#!/usr/bin/env bash
# Run the kernqueue device tests under the debug kernel built by
# scripts/prepare_debug_kdir.sh (lockdep, DEBUG_ATOMIC_SLEEP, KASAN), then
# prove those checkers were live:
#   1. require the -kqdebug kernel, with lockdep still enabled (debug_locks=1)
#   2. build kernqueue.ko and kq_debugcheck.ko against the debug tree
#   3. scripts/run_device_tests.sh; its kernel-log scan fails on lockdep,
#      KASAN and sleep-in-atomic reports (they print WARNING/BUG lines)
#   4. lockdep must still be on afterwards, and must list kernqueue's own lock
#      classes while the module is loaded (proof it was watching our locks)
#   5. kq_debugcheck.ko triggers one deliberate bug per checker; each must be
#      reported. Order: lock_order (with lockdep confirmed on just before),
#      then atomic_sleep, then oob. Any lockdep report turns lockdep off for
#      the rest of the boot, and atomic_sleep also produces one ("Invalid wait
#      context": might_fault() takes mmap_lock under a spinlock); a KASAN
#      report marks lockdep unreliable.
# Logs: build/logs/debug-<timestamp>/. Needs sudo (asks once). Leaves the
# kernel tainted by the deliberate bugs: reboot WSL afterwards.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
dbg="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}/linux-debug"
case "$(uname -r)" in
*-kqdebug) ;;
*) echo "error: running $(uname -r), not the -kqdebug kernel" >&2; exit 1 ;;
esac
out="build/logs/debug-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
status=0

sudo -v || exit 1
scripts/env_info.sh > "$out/environment.txt"
zcat /proc/config.gz | grep -E '^CONFIG_(PROVE_LOCKING|DEBUG_ATOMIC_SLEEP|KASAN|KASAN_GENERIC|KASAN_INLINE|KASAN_OUTLINE|KASAN_VMALLOC|DEBUG_LIST)=' \
	>> "$out/environment.txt"

debug_locks() { sudo awk '/debug_locks:/ {print $2}' /proc/lockdep_stats; }
if [ "$(debug_locks)" != 1 ]; then
	echo "error: lockdep is already off: something was reported earlier in this boot" >&2
	sudo dmesg > "$out/dmesg-boot.log"
	exit 1
fi
echo "lockdep active before the tests (debug_locks=1)"

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

echo "=== deliberate bugs (each must be reported)"
expect_report() { # expect_report <bug> <dmesg pattern>
	local name=$1 pattern=$2 marker="kq-debugcheck-$1-$(date +%s%N)"
	echo "$marker" | sudo tee /dev/kmsg > /dev/null
	sudo insmod "$stage/kq_debugcheck.ko" bug="$name" 2> /dev/null # fails by design
	sudo dmesg | sed -n "/$marker/,\$p" > "$out/selftest-$name.log"
	if grep -q "$pattern" "$out/selftest-$name.log"; then
		echo "--> ok: $name detected: $(grep -m1 "$pattern" "$out/selftest-$name.log" | sed 's/^\[[^]]*\] //')"
	else
		echo "--> FAILED: $name was not reported (see $out/selftest-$name.log)"
		status=1
	fi
}
# lock_order needs lockdep, which any earlier report would have switched off.
if [ "$(debug_locks)" = 1 ]; then
	echo "lockdep active before lock_order (debug_locks=1)"
else
	echo "--> FAILED: lockdep is off before lock_order; its result would mean nothing"
	status=1
fi
expect_report lock_order 'possible circular locking dependency'
expect_report atomic_sleep 'BUG: sleeping function called from invalid context'
expect_report oob 'BUG: KASAN: slab-out-of-bounds'

echo
if [ "$status" -eq 0 ]; then
	echo "DEBUG KERNEL CHECKS: PASS (logs in $out)"
else
	echo "DEBUG KERNEL CHECKS: FAIL (logs in $out)"
fi
echo "The deliberate bugs tainted this kernel; restart WSL before further use."
exit "$status"
