#!/usr/bin/env bash
# Concurrency/integrity suite against the loaded driver. Each run is one
# test_stress invocation (see tests/test_stress.c for what is checked); the
# script stops at the first failure and exits nonzero.
#
# Set KQ_SOAK_SECONDS=N to append a time-based soak run of N seconds.
set -euo pipefail

stress=${1:-build/test_stress}
dev=${KQ_DEVICE:-/dev/kernqueue}
[ -c "$dev" ] || { echo "error: $dev is not a character device (module loaded?)" >&2; exit 1; }

run() {
	echo "+ test_stress $*"
	"$stress" -q -T 300 "$@"
}

run -w 1 -r 1 -n 500000 -s 64           # SPSC: strict per-writer ordering
run -w 4 -r 4 -n 200000 -s 64           # balanced MPMC, small records
run -w 8 -r 8 -n 50000 -s 512
run -w 16 -r 1 -n 20000 -s 4096         # many writers, record = KQ_ATOMIC_MAX
run -w 1 -r 16 -n 200000 -s 32          # many readers contend for data
run -w 4 -r 4 -n 100000 -s 100 --nonblock
run -w 8 -r 8 -n 50000 -s 4096 --nonblock

if [ -n "${KQ_SOAK_SECONDS:-}" ]; then
	run -w 8 -r 8 -t "$KQ_SOAK_SECONDS" -s 256 -T $((KQ_SOAK_SECONDS + 120))
else
	echo "(soak run not requested; set KQ_SOAK_SECONDS=N to add one)"
fi
echo "stress suite: all runs passed"
