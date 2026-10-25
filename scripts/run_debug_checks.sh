#!/usr/bin/env bash
# Run the kernqueue device tests under a debug kernel built by
# scripts/prepare_debug_kdir.sh, then prove its checkers were live. Works for
# both variants: -kqdebug (lockdep, DEBUG_ATOMIC_SLEEP, KASAN) and -kqkcsan
# (DEBUG_ATOMIC_SLEEP, KCSAN strict; KCSAN is switched on via debugfs here).
#   1. require one of those kernels; with lockdep, it must still be enabled
#      (debug_locks=1)
#   2. build kernqueue.ko and kq_debugcheck.ko against the matching tree
#   3. scripts/run_device_tests.sh; its kernel-log scan fails on lockdep,
#      KASAN and sleep-in-atomic reports, and on KCSAN reports that involve
#      kernqueue (races entirely in other kernel code are counted separately)
#   4. lockdep must still be on afterwards, and must list kernqueue's own lock
#      classes while the module is loaded (proof it was watching our locks)
#   5. KCSAN kernel only: scripts/kcsan_mutant_check.sh must see KCSAN report
#      races inside a deliberately broken copy of kernqueue
#   6. kq_debugcheck.ko triggers one deliberate bug per checker; each must be
#      reported. Order: lock_order (with lockdep confirmed on just before),
#      then atomic_sleep, then oob (KASAN kernel) or data_race (KCSAN kernel;
#      informational, as it never triggered under WSL).
#      Any lockdep report turns lockdep off for the rest of the boot, and
#      atomic_sleep also produces one ("Invalid wait context": might_fault()
#      takes mmap_lock under a spinlock); a KASAN report marks it unreliable.
# Logs: build/logs/debug-<variant>-<timestamp>/. Needs sudo (asks once).
# Leaves the kernel tainted by the deliberate bugs: reboot WSL afterwards.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
base="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}"
case "$(uname -r)" in
*-kqdebug) variant=kasan; dbg="$base/linux-debug" ;;
*-kqkcsan) variant=kcsan; dbg="$base/linux-kcsan" ;;
*) echo "error: running $(uname -r), not a -kqdebug or -kqkcsan kernel" >&2; exit 1 ;;
esac
out="build/logs/debug-$variant-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
status=0

sudo -v || exit 1
scripts/env_info.sh > "$out/environment.txt"
zcat /proc/config.gz | grep -E '^CONFIG_(PROVE_LOCKING|DEBUG_ATOMIC_SLEEP|KASAN[A-Z_]*|KCSAN[A-Z_]*|DEBUG_LIST)=' \
	>> "$out/environment.txt"
echo "cmdline: $(cat /proc/cmdline)" >> "$out/environment.txt"

has_lockdep=0
zcat /proc/config.gz | grep -q '^CONFIG_PROVE_LOCKING=y' && has_lockdep=1
debug_locks() { sudo awk '/debug_locks:/ {print $2}' /proc/lockdep_stats; }
if [ "$has_lockdep" = 1 ]; then
	if [ "$(debug_locks)" != 1 ]; then
		echo "error: lockdep is already off: something was reported earlier in this boot" >&2
		sudo dmesg > "$out/dmesg-boot.log"
		exit 1
	fi
	echo "lockdep active before the tests (debug_locks=1)"
else
	echo "no lockdep in this kernel ($variant variant)"
fi

# The KCSAN kernel boots with KCSAN off (see prepare_debug_kdir.sh).
kcsan_ctl=/sys/kernel/debug/kcsan
if [ "$variant" = kcsan ]; then
	sudo mountpoint -q /sys/kernel/debug || sudo mount -t debugfs none /sys/kernel/debug
	echo on | sudo tee "$kcsan_ctl" > /dev/null
	if ! sudo grep -q 'enabled: 1' "$kcsan_ctl"; then
		echo "error: could not enable KCSAN" >&2
		exit 1
	fi
	echo "KCSAN enabled at runtime"
	sudo cat "$kcsan_ctl" > "$out/kcsan-stats-before.txt"
fi

echo "=== building against $dbg"
if ! KDIR="$dbg" scripts/build_module.sh > "$out/build.log" 2>&1; then
	cat "$out/build.log"
	exit 1
fi
stage="$HOME/.cache/kernqueue-debugcheck"
rm -rf "$stage"
mkdir -p "$stage"
cp tests/debugcheck/* "$stage/"
if ! make -C "$dbg" M="$stage" CC="$(cat "$dbg/.kq-cc")" LOCALVERSION= modules >> "$out/build.log" 2>&1; then
	cat "$out/build.log"
	exit 1
fi
grep -E '^vermagic' "$out/build.log"

echo "=== device test cycle under the debug kernel"
scripts/run_device_tests.sh 2>&1 | tee "$out/device-cycle.log"
[ "${PIPESTATUS[0]}" = 0 ] || status=1

if [ "$variant" = kcsan ]; then
	echo "=== KCSAN counters after the cycle"
	sudo cat "$kcsan_ctl" | tee "$out/kcsan-stats-after.txt" | head -n 8
fi

if [ "$has_lockdep" = 1 ]; then
	echo "=== lockdep after the cycle"
	dl=$(debug_locks)
	echo "debug_locks=$dl" | tee "$out/lockdep.txt"
	if [ "$dl" != 1 ]; then
		echo "--> FAILED: lockdep switched itself off during the cycle (it reported something)"
		status=1
	fi
	# Lock classes disappear when their module unloads, so look while it is loaded.
	scripts/load_module.sh > /dev/null || exit 1
	build/kqctl write lockdep-probe > /dev/null && build/kqctl dump > /dev/null 2>&1
	sudo grep -F '&kq.' /proc/lockdep | tee -a "$out/lockdep.txt"
	if sudo grep -qF '&kq.lock' /proc/lockdep; then
		echo "--> ok: lockdep tracks kernqueue's mutex"
	else
		echo "--> FAILED: kernqueue's mutex is not in /proc/lockdep"
		status=1
	fi
	scripts/unload_module.sh > /dev/null
fi

if [ "$variant" = kcsan ]; then
	# Required proof that KCSAN sees kernqueue's own accesses: a copy of the
	# driver with ring.count's READ_ONCE/WRITE_ONCE removed must be reported.
	echo "=== KCSAN mutation check"
	scripts/kcsan_mutant_check.sh 2>&1 | tee "$out/kcsan-mutant.log"
	[ "${PIPESTATUS[0]}" = 0 ] || status=1
fi

echo "=== deliberate bugs (each must be reported)"
expect_report() { # expect_report <bug> <dmesg pattern> [informational]
	local name=$1 pattern=$2 info=${3:-} marker="kq-debugcheck-$1-$(date +%s%N)"
	echo "$marker" | sudo tee /dev/kmsg > /dev/null
	sudo insmod "$stage/kq_debugcheck.ko" bug="$name" 2> /dev/null # fails by design
	sudo dmesg | sed -n "/$marker/,\$p" > "$out/selftest-$name.log"
	if grep -q "$pattern" "$out/selftest-$name.log"; then
		echo "--> ok: $name detected: $(grep -m1 "$pattern" "$out/selftest-$name.log" | sed 's/^\[[^]]*\] //')"
	elif [ -n "$info" ]; then
		echo "--> note: $name was not reported (informational: $info)"
	else
		echo "--> FAILED: $name was not reported (see $out/selftest-$name.log)"
		status=1
	fi
}
if [ "$has_lockdep" = 1 ]; then
	# lock_order needs lockdep, which any earlier report would have switched off.
	if [ "$(debug_locks)" = 1 ]; then
		echo "lockdep active before lock_order (debug_locks=1)"
	else
		echo "--> FAILED: lockdep is off before lock_order; its result would mean nothing"
		status=1
	fi
	expect_report lock_order 'possible circular locking dependency'
fi
expect_report atomic_sleep 'BUG: sleeping function called from invalid context'
if [ "$variant" = kasan ]; then
	expect_report oob 'BUG: KASAN: slab-out-of-bounds'
else
	# Counters around the self-test separate "not detected" (data_races
	# unchanged) from "detected but not printed".
	# This kthread race has never triggered a report under WSL (KCSAN saw no
	# value change during 44k watchpoints; cause undetermined, see
	# implementation_status.md), so it is informational. The mutation check
	# above is the required KCSAN liveness proof.
	sudo cat "$kcsan_ctl" > "$out/kcsan-stats-before-data_race.txt"
	expect_report data_race 'BUG: KCSAN: data-race in kqd_racer' \
		"does not trigger under WSL; the mutation check covers KCSAN"
	sudo cat "$kcsan_ctl" > "$out/kcsan-stats-after-data_race.txt"
	echo "    data_races counter: $(awk '/^data_races:/{print $2}' "$out/kcsan-stats-before-data_race.txt") -> $(awk '/^data_races:/{print $2}' "$out/kcsan-stats-after-data_race.txt")"
fi

echo
if [ "$status" -eq 0 ]; then
	echo "DEBUG KERNEL CHECKS: PASS (logs in $out)"
else
	echo "DEBUG KERNEL CHECKS: FAIL (logs in $out)"
fi
echo "The deliberate bugs tainted this kernel; restart WSL before further use."
exit "$status"
