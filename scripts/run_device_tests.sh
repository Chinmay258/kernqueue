#!/usr/bin/env bash
# Full device test cycle (needs sudo for insmod/rmmod; asks once):
#   1. default capacity, both wake-up modes: functional tests + stress suite
#   2. minimum capacity (4096): functional tests + two stress runs
#   3. maximum capacity (16 MiB): functional tests
#   4. out-of-range capacity values must be rejected by insmod
#   5. with --bench: benchmark matrix for both wake-up modes and a FIFO
#   6. the kernel log since the start must contain no warnings or errors
# Logs go to build/logs/<timestamp>/. Exits nonzero if anything failed.
#
# WARNING: loads an experimental kernel module. Use a disposable VM.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
bench=0
[ "${1:-}" = "--bench" ] && bench=1
logdir="build/logs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$logdir"
status=0
marker="kernqueue-test-cycle-$(date +%s)"

sudo -v || exit 1
cleanup() { grep -q '^kernqueue ' /proc/modules && sudo rmmod kernqueue; }
trap cleanup EXIT

# Put a marker in the kernel log so only this run's messages are checked.
echo "$marker" | sudo tee /dev/kmsg >/dev/null

step() { echo; echo "=== $*"; }
check() { # check <logname> <command...>
	local log="$logdir/$1.log"
	shift
	if "$@" 2>&1 | tee "$log"; then
		echo "--> ok ($log)"
	else
		echo "--> FAILED ($log)"
		status=1
	fi
}

scripts/env_info.sh > "$logdir/environment.txt"

step "default capacity, exclusive wake-ups (default)"
scripts/load_module.sh || exit 1
check functional-default build/test_functional
check stress-default scripts/run_stress.sh build/test_stress
scripts/unload_module.sh

step "default capacity, wake-all (exclusive_wake=0)"
scripts/load_module.sh exclusive_wake=0 || exit 1
check functional-wakeall build/test_functional
check stress-wakeall scripts/run_stress.sh build/test_stress
scripts/unload_module.sh

step "minimum capacity (4096)"
scripts/load_module.sh capacity=4096 || exit 1
check functional-min build/test_functional
check stress-min build/test_stress -q -w 4 -r 4 -n 100000 -s 4096
check stress-min-nb build/test_stress -q -w 4 -r 4 -n 100000 -s 1000 --nonblock
scripts/unload_module.sh

step "maximum capacity (16 MiB)"
scripts/load_module.sh capacity=16777216 || exit 1
check functional-max build/test_functional
scripts/unload_module.sh

step "invalid parameters are rejected"
for bad in 100 4095 16777217; do
	if sudo insmod build/kernqueue.ko capacity=$bad 2>/dev/null; then
		echo "--> FAILED: capacity=$bad was accepted"
		sudo rmmod kernqueue
		status=1
	else
		echo "--> ok: capacity=$bad rejected"
	fi
done

if [ "$bench" = 1 ]; then
	step "benchmark (both wake-up modes, FIFO baseline)"
	out="$root/bench/results/$(date +%Y%m%d-%H%M%S)"
	for mode in 1 0; do
		targets=kernqueue
		[ "$mode" = 1 ] && targets="kernqueue fifo"
		scripts/load_module.sh exclusive_wake=$mode || exit 1
		check "bench-wake$mode" env KQ_BENCH_OUT="$out" KQ_BENCH_TARGETS="$targets" \
			bench/run_bench.sh build/kq_bench
		scripts/unload_module.sh
	done
fi

step "kernel log"
sudo dmesg | sed -n "/$marker/,\$p" > "$logdir/dmesg.log"
# Lines that are expected on every load: the out-of-tree taint notice.
grep -v 'loading out-of-tree module taints kernel' "$logdir/dmesg.log" > "$logdir/dmesg-filtered.log"
if grep -niE 'warning|bug|oops|error|invalid|mismatch|lockdep|circular locking|blocked for more than|call trace' "$logdir/dmesg-filtered.log"; then
	echo "--> FAILED: unexpected kernel log lines (see $logdir/dmesg.log)"
	status=1
else
	echo "--> ok: no unexpected lines ($(grep -c kernqueue "$logdir/dmesg.log") kernqueue lines)"
fi

echo
if [ "$status" -eq 0 ]; then
	echo "DEVICE TEST CYCLE: PASS (logs in $logdir)"
else
	echo "DEVICE TEST CYCLE: FAIL (logs in $logdir)"
fi
exit "$status"
