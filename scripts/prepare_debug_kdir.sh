#!/usr/bin/env bash
# Build a debug variant of the running WSL2 kernel for running the device
# tests under kernel sanitizers. Needs the stock tree from prepare_wsl_kdir.sh
# (its git checkout is reused through a worktree, so nothing is downloaded and
# the stock tree is not touched).
#
#   scripts/prepare_debug_kdir.sh [kasan|kcsan]      (default: kasan)
#
# Both variants enable DEBUG_ATOMIC_SLEEP, plus:
#   kasan  lockdep (PROVE_LOCKING), KASAN (generic) + KASAN_VMALLOC, DEBUG_LIST
#          -> ~/kq-kbuild/linux-debug, release suffix -kqdebug
#   kcsan  KCSAN in strict mode, starting disabled
#          -> ~/kq-kbuild/linux-kcsan, release suffix -kqkcsan
# KASAN and KCSAN cannot be combined. KCSAN_STRICT is used because the default
# mode deliberately ignores races between plain aligned writes and marked
# (READ_ONCE) reads, which is exactly the discipline kernqueue relies on.
# The KCSAN kernel leaves out lockdep and KCSAN's boot-time selftest and does
# not enable KCSAN during boot: with all of that, WSL timed out waiting for
# the VM to boot (HCS_E_CONNECTION_TIMEOUT). run_debug_checks.sh switches
# KCSAN on through debugfs right before the tests.
#
#   1. git worktree of the stock tree
#   2. /proc/config.gz plus the options above, under a distinct release name
#      so `uname -r` shows which kernel booted and no stock modules can be
#      loaded into it (their struct layouts differ)
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

variant=${1:-kasan}
base="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}"
stock="$base/linux"
cc=$(cat "$stock/.kq-cc" 2>/dev/null || echo gcc)
common=(-e DEBUG_KERNEL -e DEBUG_ATOMIC_SLEEP)
required=(DEBUG_ATOMIC_SLEEP)
case "$variant" in
kasan)
	dir="$base/linux-debug"
	suffix="-microsoft-standard-WSL2-kqdebug"
	extra=(-e PROVE_LOCKING -e KASAN -e KASAN_GENERIC -e KASAN_VMALLOC -e DEBUG_LIST)
	required+=(PROVE_LOCKING LOCKDEP DEBUG_LOCK_ALLOC DEBUG_MUTEXES DEBUG_SPINLOCK
		   KASAN KASAN_GENERIC KASAN_VMALLOC DEBUG_LIST)
	;;
kcsan)
	dir="$base/linux-kcsan"
	suffix="-microsoft-standard-WSL2-kqkcsan"
	extra=(-d KASAN -d PROVE_LOCKING -e KCSAN -e KCSAN_STRICT
	       -d KCSAN_SELFTEST -d KCSAN_EARLY_ENABLE)
	required+=(KCSAN KCSAN_STRICT)
	;;
*)
	echo "usage: $0 [kasan|kcsan]" >&2
	exit 2
	;;
esac

[ -d "$stock/.git" ] || { echo "error: run scripts/prepare_wsl_kdir.sh first" >&2; exit 1; }
if [ ! -d "$dir" ]; then
	git -C "$stock" worktree add --detach "$dir" HEAD
fi
cd "$dir"

if [ ! -f .config ]; then
	zcat /proc/config.gz > .config
	scripts/config --set-str LOCALVERSION "$suffix" "${common[@]}" "${extra[@]}"
	make -s CC="$cc" LOCALVERSION= olddefconfig
fi

# olddefconfig silently drops options whose dependencies are not met; check.
missing=0
for opt in "${required[@]}"; do
	if grep -q "^CONFIG_$opt=y" .config; then
		echo "   CONFIG_$opt=y"
	else
		echo "error: CONFIG_$opt is not enabled" >&2
		missing=1
	fi
done
[ "$missing" = 0 ] || exit 1
if [ "$variant" = kcsan ]; then
	for opt in PROVE_LOCKING KCSAN_SELFTEST KCSAN_EARLY_ENABLE; do
		if grep -q "^CONFIG_$opt=y" .config; then
			echo "error: CONFIG_$opt must be off for the KCSAN kernel to boot under WSL" >&2
			exit 1
		fi
	done
fi
grep -E '^CONFIG_(KASAN_(INLINE|OUTLINE)|KCSAN_[A-Z_]+)=' .config || true

echo ">> building bzImage ($variant) with $cc on $(nproc) CPUs"
make CC="$cc" LOCALVERSION= -j"$(nproc)" bzImage
make CC="$cc" LOCALVERSION= modules_prepare
cp vmlinux.symvers Module.symvers
echo "$cc" > .kq-cc

built=$(make -s CC="$cc" LOCALVERSION= kernelrelease)
echo ">> ready: KDIR=$dir"
echo ">> kernel image: $dir/arch/x86/boot/bzImage (release $built)"
