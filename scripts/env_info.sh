#!/usr/bin/env bash
# Print the facts needed to interpret a test or benchmark result.
set -u

echo "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "kernel:      $(uname -r) ($(uname -m))"
echo "os:          $(. /etc/os-release && echo "$PRETTY_NAME")"
if grep -qi microsoft /proc/version; then
	echo "virtualized: WSL2 (Hyper-V VM)"
fi
echo "cpu:         $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
echo "cpus:        $(nproc) online"
echo "memory:      $(free -h | awk '/^Mem:/ {print $2}')"
echo "gcc:         $(gcc --version | head -n1)"
if [ -r /sys/module/kernqueue/parameters/capacity ]; then
	echo "kernqueue:   loaded, capacity=$(cat /sys/module/kernqueue/parameters/capacity)"
else
	echo "kernqueue:   not loaded"
fi
