#!/usr/bin/env bash
# Prepare a kernel build tree that matches the running WSL2 kernel, so that
# out-of-tree modules can be compiled against it. Microsoft does not ship
# headers for WSL kernels and Ubuntu's linux-headers packages do not match.
#
#   1. Shallow-clone microsoft/WSL2-Linux-Kernel at the tag for `uname -r`.
#   2. Use the running kernel's own configuration (/proc/config.gz).
#   3. Show how the resulting .config differs from the running one. It must
#      match in everything that affects ABI: for example CONFIG_DEBUG_INFO_BTF
#      adds fields to struct module, so a mismatch would corrupt THIS_MODULE.
#   4. Build vmlinux and the in-tree modules. The full build is what produces
#      Module.symvers, which CONFIG_MODVERSIONS=y needs for symbol CRCs.
#
# Runs entirely as the invoking user. Needs: git flex bison bc cpio libelf-dev
# libssl-dev dwarves (pahole) and a gcc; gcc-11 is preferred because the WSL
# kernel itself was built with gcc 11. cpio is used by CONFIG_IKHEADERS.
#
# Rerunning after a failure resumes the build: an existing .config is kept,
# so make only rebuilds what is missing.
#
# Environment: KQ_KBUILD_DIR (default ~/kq-kbuild), KQ_CC (default gcc-11 if
# installed, else gcc), KQ_ALLOW_CONFIG_DIFF=1 to continue past suspicious
# config differences.
set -euo pipefail

krel=$(uname -r)
case "$krel" in
*-microsoft-standard-WSL2) ;;
*)
	echo "error: '$krel' is not a Microsoft WSL2 kernel; on a regular distro" >&2
	echo "       install the matching linux-headers package instead." >&2
	exit 1
	;;
esac
ver=${krel%%-microsoft*}
tag="linux-msft-wsl-$ver"
dir="${KQ_KBUILD_DIR:-$HOME/kq-kbuild}/linux"
cc=${KQ_CC:-$(command -v gcc-11 >/dev/null && echo gcc-11 || echo gcc)}

for tool in git flex bison bc cpio pahole "$cc"; do
	command -v "$tool" >/dev/null || { echo "error: missing tool: $tool" >&2; exit 1; }
done
[ -r /usr/include/libelf.h ] || { echo "error: libelf-dev is not installed" >&2; exit 1; }
[ -r /usr/include/openssl/ssl.h ] || { echo "error: libssl-dev is not installed" >&2; exit 1; }

if [ ! -d "$dir/.git" ]; then
	echo ">> cloning $tag into $dir"
	git clone --depth 1 --branch "$tag" \
		https://github.com/microsoft/WSL2-Linux-Kernel "$dir"
fi
cd "$dir"
echo ">> source: $(git describe --tags 2>/dev/null || git rev-parse --short HEAD)"

if [ ! -f .config ]; then
	zcat /proc/config.gz > .config
	make -s CC="$cc" olddefconfig
fi

echo ">> .config differences from the running kernel:"
scripts/diffconfig <(zcat /proc/config.gz) .config | tee kq-config.diff || true
# Expected, ABI-neutral differences:
#   - toolchain version strings and capability probes (CC_HAS_*, ...);
#   - options that are off on one side and absent on the other ("+SLS n");
#   - GCC_PLUGINS, but only while no individual plugin and no RANDSTRUCT
#     variant is enabled in the running kernel (it vanishes when the gcc
#     plugin headers are not installed);
#   - GCC_ASM_GOTO_OUTPUT_WORKAROUND: adds 'volatile' to asm goto to dodge a
#     gcc < 11.5 bug; code generation only.
# Anything else could change the kernel's ABI and is flagged.
tolerated='^ ?(CC_VERSION_TEXT|GCC_VERSION|AS_VERSION|LD_VERSION|PAHOLE_VERSION) '
tolerated+='|^[ +-]?(CC_HAS_|CC_CAN_|AS_HAS_|LD_CAN_|TOOLS_SUPPORT_|PAHOLE_HAS_)[A-Z0-9_]* '
tolerated+='|^[+-][A-Z0-9_]+ n$'
tolerated+='|^-GCC_ASM_GOTO_OUTPUT_WORKAROUND y$'
if ! zcat /proc/config.gz | grep -qE '^CONFIG_(GCC_PLUGIN_[A-Z_]+|RANDSTRUCT_FULL|RANDSTRUCT_PERFORMANCE)=y'; then
	tolerated+='|^-GCC_PLUGINS y$'
fi
suspicious=$(grep -vE "$tolerated" kq-config.diff || true)
if [ -n "$suspicious" ]; then
	echo "warning: config differences beyond toolchain versions:" >&2
	echo "$suspicious" >&2
	if [ "${KQ_ALLOW_CONFIG_DIFF:-0}" != 1 ]; then
		echo "error: refusing to continue; review the lines above and rerun" >&2
		echo "       with KQ_ALLOW_CONFIG_DIFF=1 if they cannot affect the ABI." >&2
		exit 1
	fi
fi

# LOCALVERSION= (set but empty) stops scripts/setlocalversion from appending
# "+" because the checkout is not at an annotated tag; with the "+" the
# release string, and so every module's vermagic, would not match uname -r.
echo ">> building with $cc on $(nproc) CPUs (this takes a while)"
make CC="$cc" LOCALVERSION= -j"$(nproc)"

built=$(make -s CC="$cc" LOCALVERSION= kernelrelease)
if [ "$built" != "$krel" ]; then
	echo "error: tree builds '$built' but the running kernel is '$krel'" >&2
	exit 1
fi
[ -s Module.symvers ] || { echo "error: Module.symvers was not produced" >&2; exit 1; }
# Record the compiler: modules must be built with the same one as the tree.
echo "$cc" > .kq-cc
echo ">> ready: KDIR=$dir (kernelrelease $built, CC=$cc)"
