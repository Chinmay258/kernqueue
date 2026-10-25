#!/usr/bin/env bash
# Load build/kernqueue.ko (needs sudo) and give the invoking user's group
# read/write access to /dev/kernqueue so tests can run unprivileged.
#
#   scripts/load_module.sh [capacity=BYTES]
#
# WARNING: an experimental module runs with full kernel privileges. A bug can
# crash or hang the whole (virtual) machine. Use a disposable VM.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
ko="$root/build/kernqueue.ko"

[ -f "$ko" ] || { echo "error: $ko not found; run 'make module' first" >&2; exit 1; }
if grep -q '^kernqueue ' /proc/modules; then
	echo "error: kernqueue is already loaded; run scripts/unload_module.sh" >&2
	exit 1
fi

# Refuse a module built for a different kernel instead of letting insmod fail obscurely.
vermagic=$(modinfo -F vermagic "$ko")
if [ "${vermagic%% *}" != "$(uname -r)" ]; then
	echo "error: module vermagic '$vermagic' does not match running kernel $(uname -r)" >&2
	exit 1
fi

echo ">> insmod $ko $*"
sudo insmod "$ko" "$@"

# devtmpfs creates the node as root:root 0600; wait for it, then open it up
# to the current user's primary group.
for _ in $(seq 50); do
	[ -c /dev/kernqueue ] && break
	sleep 0.1
done
[ -c /dev/kernqueue ] || { echo "error: /dev/kernqueue did not appear" >&2; exit 1; }
sudo chgrp "$(id -gn)" /dev/kernqueue
sudo chmod 0660 /dev/kernqueue
ls -l /dev/kernqueue
dmesg | grep kernqueue | tail -n 1 || true
