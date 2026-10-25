#!/usr/bin/env bash
# KCSAN mutation check: prove KCSAN would catch kernqueue's own bug class.
#
# Builds a deliberately broken copy of the driver in which the lockless
# accesses to ring.count lose their markings (READ_ONCE -> plain read in the
# wait conditions and poll, WRITE_ONCE -> plain store in kq_ring.h), loads it
# instead of the real driver, runs the concurrent stress tests on it and
# requires KCSAN to report a race whose racing access is inside kernqueue.
# A clean result for the real driver only means something if this fires.
#
# Needs the -kqkcsan kernel (scripts/prepare_debug_kdir.sh kcsan) and sudo.
# Logs: build/logs/kcsan-mutant-<timestamp>/.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
case "$(uname -r)" in
*-kqkcsan) ;;
*) echo "error: needs the -kqkcsan kernel, running $(uname -r)" >&2; exit 1 ;;
esac
kdir="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}/linux-kcsan"
out="build/logs/kcsan-mutant-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
stage="$HOME/.cache/kernqueue-kcsan-mutant"

sudo -v || exit 1
grep -q '^kernqueue ' /proc/modules && { echo "error: unload kernqueue first" >&2; exit 1; }

echo "=== building the mutant"
rm -rf "$stage"
mkdir -p "$stage"
cp -r driver include "$stage/"
sed -i 's/READ_ONCE(dev->ring.count)/dev->ring.count/' "$stage/driver/kernqueue.c"
sed -i 's/#define KQ_WRITE_ONCE(x, val) WRITE_ONCE(x, val)/#define KQ_WRITE_ONCE(x, val) ((x) = (val))/' "$stage/driver/kq_ring.h"
diff -u driver/kernqueue.c "$stage/driver/kernqueue.c" > "$out/mutation.diff"
diff -u driver/kq_ring.h "$stage/driver/kq_ring.h" >> "$out/mutation.diff"
if grep -q 'READ_ONCE(dev->ring.count)' "$stage/driver/kernqueue.c" ||
   ! grep -q '((x) = (val))' "$stage/driver/kq_ring.h"; then
	echo "error: mutation not applied" >&2
	exit 1
fi
echo "mutation: $(grep -c '^[-+][^-+]' "$out/mutation.diff") changed lines (see $out/mutation.diff)"
make -C "$kdir" M="$stage/driver" CC="$(cat "$kdir/.kq-cc")" LOCALVERSION= modules > "$out/build.log" 2>&1 ||
	{ cat "$out/build.log"; exit 1; }

sudo mountpoint -q /sys/kernel/debug || sudo mount -t debugfs none /sys/kernel/debug
echo on | sudo tee /sys/kernel/debug/kcsan > /dev/null
sudo grep -q 'enabled: 1' /sys/kernel/debug/kcsan || { echo "error: KCSAN not enabled" >&2; exit 1; }

echo "=== stress on the mutant"
marker="kq-kcsan-mutant-$(date +%s%N)"
echo "$marker" | sudo tee /dev/kmsg > /dev/null
sudo insmod "$stage/driver/kernqueue.ko"
for _ in $(seq 50); do [ -c /dev/kernqueue ] && break; sleep 0.1; done
sudo chgrp "$(id -gn)" /dev/kernqueue && sudo chmod 0660 /dev/kernqueue
{
	build/test_stress -q -w 4 -r 4 -n 50000 -s 64
	build/test_stress -q -w 4 -r 4 -n 50000 -s 100 --nonblock
	build/test_stress -q -w 1 -r 16 -n 50000 -s 32
} 2>&1 | tee "$out/stress.log"
sudo rmmod kernqueue

sudo dmesg | sed -n "/$marker/,\$p" > "$out/dmesg.log"
grep -q "$marker" "$out/dmesg.log" || { echo "error: marker lost from the kernel log" >&2; exit 1; }
: > "$out/dmesg-kcsan.log"
awk -v kcsan="$out/dmesg-kcsan.log" '
	/BUG: KCSAN:/ { inblk = 1; blk = $0 "\n"; next }
	inblk && /\] =+$/ { printf "%s", blk >> kcsan; inblk = 0; next }
	inblk { blk = blk $0 "\n"; next }
' "$out/dmesg.log"
python3 scripts/kcsan_attribute.py "$out/dmesg-kcsan.log" > "$out/attribution.txt"
cat "$out/attribution.txt"

echo
if grep -q '\[kernqueue\]' "$out/attribution.txt"; then
	echo "KCSAN MUTANT CHECK: PASS (KCSAN reported races inside the mutant kernqueue)"
	exit 0
fi
echo "KCSAN MUTANT CHECK: FAIL (no race attributed to the mutant; KCSAN may not see this bug class)"
exit 1
