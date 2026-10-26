#!/usr/bin/env bash
# Mutation check for the exclusive wake-up logic: prove that the three
# regression tests written for it actually fail when the logic is broken.
#
# For each mutant, a copy of the driver is built with one rule removed,
# loaded with exclusive_wake=1, and its targeted test must FAIL in every one
# of KQ_MUTANT_REPS runs (default 3). The real driver must PASS the same tests
# (control). Mutants only break wake-up liveness (blocked threads stay
# asleep); the stuck test threads are killable, so rmmod still works.
#
#   M1 need filter removed   kq_writer_wake() accepts every writer  -> wake_skips_unfit_writer
#   M2 reader hand-off off   kq_pass_on_readers() does nothing      -> wake_hands_on_to_next_reader
#   M3 writer hand-off off   kq_pass_on_writers() does nothing      -> wake_hands_on_to_next_writer
#
# Needs sudo (asks once) and a prepared kernel tree (see build_module.sh).
# Logs: build/logs/wake-mutant-<timestamp>/.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
reps=${KQ_MUTANT_REPS:-3}
if [ -z "${KDIR:-}" ]; then
	if [ -e "/lib/modules/$(uname -r)/build/Makefile" ]; then
		KDIR="/lib/modules/$(uname -r)/build"
	else
		KDIR="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}/linux"
	fi
fi
cc=${KQ_CC:-$(cat "$KDIR/.kq-cc" 2>/dev/null || echo gcc)}
out="build/logs/wake-mutant-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
status=0

[ -x build/test_functional ] || { echo "error: run 'make user' first" >&2; exit 1; }
sudo -v || exit 1
grep -q '^kernqueue ' /proc/modules && { echo "error: unload kernqueue first" >&2; exit 1; }
cleanup() { grep -q '^kernqueue ' /proc/modules && sudo rmmod kernqueue; }
trap cleanup EXIT

load() { # load <ko>
	sudo insmod "$1" exclusive_wake=1 || return 1
	for _ in $(seq 50); do [ -c /dev/kernqueue ] && break; sleep 0.1; done
	sudo chgrp "$(id -gn)" /dev/kernqueue && sudo chmod 0660 /dev/kernqueue
}

build_variant() { # build_variant <name> <file> <sed expression> <must-disappear>
	local name=$1 file=$2 expr=$3 gone=$4 stage="$HOME/.cache/kernqueue-wake-$1"
	rm -rf "$stage"
	mkdir -p "$stage"
	cp -r driver include "$stage/"
	if [ -n "$expr" ]; then
		sed -i "$expr" "$stage/driver/$file"
		if grep -qF "$gone" "$stage/driver/$file"; then
			echo "error: mutation $name was not applied" >&2
			return 1
		fi
		diff -u "driver/$file" "$stage/driver/$file" > "$out/$name.diff"
	fi
	make -C "$KDIR" M="$stage/driver" CC="$cc" LOCALVERSION= modules > "$out/$name-build.log" 2>&1 ||
		{ cat "$out/$name-build.log"; return 1; }
	echo "$stage/driver/kernqueue.ko"
}

# name | test | sed expression | text that must be gone afterwards
variants=(
	"M1|wake_skips_unfit_writer|s/if (!kq_has_space(w->dev, w->need))/if (0)/|if (!kq_has_space(w->dev, w->need))"
	"M2|wake_hands_on_to_next_reader|s/if (exclusive_wake \&\& kq_has_data(dev))/if (0)/|if (exclusive_wake && kq_has_data(dev))"
	"M3|wake_hands_on_to_next_writer|s/if (exclusive_wake \&\& kq_has_space(dev, 1))/if (0)/|if (exclusive_wake && kq_has_space(dev, 1))"
)

echo "=== control: real driver must pass the targeted tests"
ko=$(build_variant control kernqueue.c "" "") || exit 1
load "$ko" || exit 1
for v in "${variants[@]}"; do
	IFS='|' read -r name test _ _ <<<"$v"
	if build/test_functional "$test" > "$out/control-$test.log" 2>&1; then
		echo "--> ok: $test passes on the real driver"
	else
		echo "--> FAILED: $test fails on the real driver (see $out/control-$test.log)"
		status=1
	fi
done
sudo rmmod kernqueue

for v in "${variants[@]}"; do
	IFS='|' read -r name test expr gone <<<"$v"
	echo "=== mutant $name (target: $test)"
	ko=$(build_variant "$name" kernqueue.c "$expr" "$gone") || exit 1
	load "$ko" || exit 1
	caught=0
	for rep in $(seq "$reps"); do
		if build/test_functional "$test" > "$out/$name-run$rep.log" 2>&1; then
			echo "    run $rep: test PASSED on the mutant (not detected)"
		else
			echo "    run $rep: test failed as required: $(grep -m1 -E 'FAIL|TIMEOUT|STUCK' "$out/$name-run$rep.log")"
			caught=$((caught + 1))
		fi
	done
	sudo rmmod kernqueue
	if [ "$caught" -eq "$reps" ]; then
		echo "--> ok: $name detected in $caught/$reps runs"
	else
		echo "--> FAILED: $name detected in only $caught/$reps runs"
		status=1
	fi
done

echo
if [ "$status" -eq 0 ]; then
	echo "WAKE MUTANT CHECK: PASS (logs in $out)"
else
	echo "WAKE MUTANT CHECK: FAIL (logs in $out)"
fi
exit "$status"
