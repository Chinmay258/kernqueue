#!/usr/bin/env bash
# Unload kernqueue (needs sudo). rmmod fails with EBUSY while any process
# still has /dev/kernqueue open; that is the module refcount doing its job.
set -euo pipefail

if ! grep -q '^kernqueue ' /proc/modules; then
	echo "kernqueue is not loaded"
	exit 0
fi
sudo rmmod kernqueue
dmesg | grep kernqueue | tail -n 1 || true
