#!/usr/bin/env bash
# Self-test of the stress harness against a named pipe. No driver needed.
#
# Pipes give the guarantees the harness relies on (FIFO order, atomic writes
# of <= PIPE_BUF bytes), so the normal runs must pass; if they fail, the
# harness is broken. The --inject runs deliberately break one guarantee and
# must be detected (nonzero exit); if one passes, the checker is too weak.
# This validates the harness only, not kernqueue.
set -euo pipefail

stress=${1:-build/test_stress}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

expect_pass() {
	local fifo="$dir/q$RANDOM"
	mkfifo "$fifo"
	echo "+ expect PASS: test_stress $*"
	"$stress" -d "$fifo" -q -T 60 "$@"
}

expect_fail() {
	local fifo="$dir/q$RANDOM" rc=0
	mkfifo "$fifo"
	echo "+ expect FAIL: test_stress $*"
	"$stress" -d "$fifo" -q -T 60 "$@" 2>"$dir/err" || rc=$?
	if [ "$rc" -ne 1 ]; then
		echo "harness self-test FAILED: fault not detected (exit $rc)" >&2
		exit 1
	fi
	echo "  detected: $(head -n 1 "$dir/err")"
}

expect_pass -w 1 -r 1 -n 200000 -s 64
expect_pass -w 4 -r 4 -n 50000 -s 64
expect_pass -w 8 -r 2 -n 20000 -s 4096
expect_pass -w 2 -r 8 -n 50000 -s 100 --nonblock

expect_fail -w 1 -r 1 -n 1000 --inject dup
expect_fail -w 2 -r 3 -n 1000 --inject dup
expect_fail -w 1 -r 1 -n 1000 --inject skip
expect_fail -w 2 -r 3 -n 1000 --inject skip
expect_fail -w 2 -r 2 -n 1000 --inject corrupt
expect_fail -w 2 -r 2 -n 1000 -s 32 --inject corrupt
expect_fail -w 8 -r 4 -n 50000 -s 4096 --inject split

echo "harness self-test: all checks behaved as expected"
