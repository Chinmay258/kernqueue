#!/usr/bin/env bash
# Build a debug variant of the running WSL2 kernel with lockdep, sleep-in-
# atomic checking and KASAN, for running the device tests under kernel
# sanitizers. Needs the stock tree from prepare_wsl_kdir.sh (its git checkout
# is reused through a worktree, so nothing is downloaded and the stock tree
# is not touched).
#
#   1. git worktree of the stock tree at ~/kq-kbuild/linux-debug
#   2. /proc/config.gz plus the debug options below, under a distinct release
#      name (-kqdebug) so `uname -r` shows which kernel booted and no stock
#      modules can be loaded into it (their struct layouts differ)
#   3. build bzImage and modules_prepare. Only vmlinux's exports are needed to
#      build kernqueue.ko, so vmlinux.symvers serves as Module.symvers and the
#      ~700 in-tree modules are skipped.
#
# Booting it changes system configuration: add to %UserProfile%\.wslconfig
#   [wsl2]
#   kernel=C:\\path\\to\\bzImage
# then `wsl --shutdown`. Remove the line (or file) and shut down again to
# return to the stock kernel.
set -euo pipefail

base="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}"
stock="$base/linux"
dir="$base/linux-debug"
cc=$(cat "$stock/.kq-cc" 2>/dev/null || echo gcc)
release_suffix="-microsoft-standard-WSL2-kqdebug"

[ -d "$stock/.git" ] || { echo "error: run scripts/prepare_wsl_kdir.sh first" >&2; exit 1; }
if [ ! -d "$dir" ]; then
	git -C "$stock" worktree add --detach "$dir" HEAD
fi
cd "$dir"

if [ ! -f .config ]; then
	zcat /proc/config.gz > .config
	scripts/config --set-str LOCALVERSION "$release_suffix" \
		-e DEBUG_KERNEL \
		-e PROVE_LOCKING \
		-e DEBUG_ATOMIC_SLEEP \
		-e KASAN -e KASAN_GENERIC -e KASAN_VMALLOC \
		-e DEBUG_LIST
	make -s CC="$cc" LOCALVERSION= olddefconfig
fi

# olddefconfig silently drops options whose dependencies are not met; check.
missing=0
for opt in PROVE_LOCKING LOCKDEP DEBUG_LOCK_ALLOC DEBUG_MUTEXES DEBUG_SPINLOCK \
	   DEBUG_ATOMIC_SLEEP KASAN KASAN_GENERIC KASAN_VMALLOC DEBUG_LIST; do
	if grep -q "^CONFIG_$opt=y" .config; then
		echo "   CONFIG_$opt=y"
	else
		echo "error: CONFIG_$opt is not enabled" >&2
		missing=1
	fi
done
[ "$missing" = 0 ] || exit 1
grep -E '^CONFIG_KASAN_(INLINE|OUTLINE)=y' .config || true

echo ">> building bzImage with $cc on $(nproc) CPUs"
make CC="$cc" LOCALVERSION= -j"$(nproc)" bzImage
make CC="$cc" LOCALVERSION= modules_prepare
cp vmlinux.symvers Module.symvers
echo "$cc" > .kq-cc

built=$(make -s CC="$cc" LOCALVERSION= kernelrelease)
echo ">> ready: KDIR=$dir"
echo ">> kernel image: $dir/arch/x86/boot/bzImage (release $built)"
