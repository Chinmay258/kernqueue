#!/usr/bin/env bash
# Build kernqueue.ko against a prepared kernel tree and copy it to build/.
#
# KDIR selects the tree. Default: /lib/modules/$(uname -r)/build if it exists,
# else the tree made by scripts/prepare_wsl_kdir.sh (~/kq-kbuild/linux).
# The module is compiled with the same CC as the tree (recorded in
# $KDIR/.kq-cc), because modules built with a different compiler can break
# (e.g. GCC plugins are compiler-version specific).
#
# Kbuild cannot handle spaces in M=. If the repository path contains spaces,
# driver/ and include/ are staged into a space-free directory first.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${KDIR:-}" ]; then
	if [ -e "/lib/modules/$(uname -r)/build/Makefile" ]; then
		KDIR="/lib/modules/$(uname -r)/build"
	else
		KDIR="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}/linux"
	fi
fi
if [ ! -e "$KDIR/Makefile" ] || [ ! -s "$KDIR/Module.symvers" ]; then
	echo "error: '$KDIR' is not a prepared kernel tree (no Makefile or Module.symvers)." >&2
	echo "       Install your distro's kernel headers, or on WSL2 run" >&2
	echo "       scripts/prepare_wsl_kdir.sh, or set KDIR=..." >&2
	exit 1
fi
cc=${KQ_CC:-$(cat "$KDIR/.kq-cc" 2>/dev/null || echo gcc)}

src="$root"
if [[ "$root" =~ [[:space:]] ]]; then
	src="${KQ_STAGE_DIR:-$HOME/.cache/kernqueue-build}"
	echo ">> repository path contains spaces; staging sources in $src"
	rm -rf "$src/driver" "$src/include"
	mkdir -p "$src"
	cp -r "$root/driver" "$root/include" "$src/"
fi

# KQ_SPARSE=1 runs sparse on every source file (C=2). A user-local sparse
# under ~/kq-kbuild/tools is used if none is installed system-wide.
check=()
if [ -n "${KQ_SPARSE:-}" ]; then
	command -v sparse >/dev/null || PATH="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}/tools/root/usr/bin:$PATH"
	command -v sparse >/dev/null || { echo "error: sparse not found (apt install sparse)" >&2; exit 1; }
	check=(C=2)
fi

echo ">> building against $KDIR with CC=$cc (W=1 extra warnings${check:+, sparse})"
# LOCALVERSION= matches how prepare_wsl_kdir.sh built the tree (no "+").
make -C "$KDIR" M="$src/driver" CC="$cc" LOCALVERSION= W=1 "${check[@]}" modules

# Module BTF is "split" BTF: type IDs and string offsets continue from the
# vmlinux BTF it was generated against. Kbuild used this tree's vmlinux,
# whose BTF differs from the running kernel's when pahole versions differ
# (WSL: 1.21 vs 1.25 here), and the kernel then rejects it ("Kernel module
# BTF mismatch detected"). Regenerate it against the running kernel's own
# BTF so BPF tooling can see the module's types.
ko="$src/driver/kernqueue.ko"
if [ "${KQ_RUNNING_BTF:-1}" = 1 ] && [ -r /sys/kernel/btf/vmlinux ] &&
   command -v pahole >/dev/null && [ -x "$KDIR/scripts/pahole-flags.sh" ]; then
	echo ">> regenerating module BTF against /sys/kernel/btf/vmlinux"
	objcopy --remove-section=.BTF "$ko"
	LLVM_OBJCOPY=objcopy pahole -J $(PAHOLE=pahole "$KDIR/scripts/pahole-flags.sh") \
		--btf_base /sys/kernel/btf/vmlinux "$ko"
fi

mkdir -p "$root/build"
cp "$ko" "$root/build/kernqueue.ko"
echo ">> built $root/build/kernqueue.ko"
modinfo "$root/build/kernqueue.ko" | grep -E '^(vermagic|parm|license):'
