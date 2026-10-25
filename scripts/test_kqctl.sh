#!/usr/bin/env bash
# Smoke test of the kqctl client against the loaded driver. Uses the device
# exclusively (resets it). Exits nonzero on the first failed check.
set -uo pipefail

k=${1:-build/kqctl}
fails=0

check() { # check <description> <command...>
	local what=$1
	shift
	if "$@"; then
		echo "ok    $what"
	else
		echo "FAIL  $what"
		fails=$((fails + 1))
	fi
}
used() { "$k" stats | awk '$1 == "used" {print $2}'; }

check "reset and clear-stats succeed" bash -c "'$k' reset && '$k' clear-stats"
check "empty queue reports POLLOUT only" test "$("$k" poll 0)" = "POLLOUT"
check "write TEXT queues the bytes" "$k" write "hello kernqueue"
check "stats shows 15 bytes used" test "$(used)" = 15
check "poll reports POLLIN and POLLOUT" test "$("$k" poll 0)" = "POLLIN POLLOUT"
check "read N returns the text" test "$("$k" read 100 2>/dev/null)" = "hello kernqueue"
check "non-blocking read on empty queue fails" bash -c "! '$k' -n read 1 2>/dev/null"
check "write - copies stdin" bash -c "printf 'abc' | '$k' write -"
check "dump drains the queue" test "$("$k" dump 2>/dev/null)" = "abc"
check "queue is empty again" test "$(used)" = 0
check "reset discards queued data" bash -c "'$k' write xyz && '$k' reset && test \"\$('$k' stats | awk '\$1==\"used\"{print \$2}')\" = 0"
check "bad command exits with usage error 2" bash -c "'$k' frobnicate >/dev/null 2>&1; test \$? = 2"
check "invalid read size is rejected" bash -c "! '$k' read 0 2>/dev/null"

if [ "$fails" -eq 0 ]; then
	echo "test_kqctl: PASS"
else
	echo "test_kqctl: FAIL ($fails checks)"
	exit 1
fi
