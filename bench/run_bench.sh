#!/usr/bin/env bash
# Benchmark matrix against the loaded kernqueue and/or a named-pipe baseline
# with the same capacity (a Linux pipe holds 64 KiB by default, kernqueue's
# default too). Each configuration runs KQ_BENCH_REPS times (default 3) for
# KQ_BENCH_SECONDS (default 5) after a 1 s warm-up.
#
# kernqueue rows are labelled by wake-up mode: kq-excl (exclusive_wake=Y) or
# kq-all (exclusive_wake=N). Output: KQ_BENCH_OUT (default
# bench/results/<timestamp>/) gets results.csv (appended to if present),
# environment.txt and summary.md (medians over all rows so far).
# KQ_BENCH_TARGETS selects "kernqueue", "fifo" or both (default).
set -euo pipefail

bench=${1:-build/kq_bench}
dev=${KQ_DEVICE:-/dev/kernqueue}
reps=${KQ_BENCH_REPS:-3}
secs=${KQ_BENCH_SECONDS:-5}
targets=${KQ_BENCH_TARGETS:-kernqueue fifo}
root=$(cd "$(dirname "$0")/.." && pwd)
out=${KQ_BENCH_OUT:-$root/bench/results/$(date +%Y%m%d-%H%M%S)}

[ -c "$dev" ] || { echo "error: $dev missing (module loaded?)" >&2; exit 1; }
cap=$(cat /sys/module/kernqueue/parameters/capacity)
mode=$(cat /sys/module/kernqueue/parameters/exclusive_wake)
[ "$cap" = 65536 ] || echo "note: capacity=$cap, but the FIFO baseline holds 65536 bytes"

mkdir -p "$out"
if [ ! -f "$out/environment.txt" ]; then
	"$root/scripts/env_info.sh" > "$out/environment.txt"
	echo "reps=$reps seconds=$secs warmup=1 capacity=$cap" >> "$out/environment.txt"
fi
[ -f "$out/results.csv" ] || "$bench" --header > "$out/results.csv"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fifo="$tmp/fifo"
mkfifo "$fifo"

# writers readers size [extra args]
configs=(
	"1 1 64"
	"1 1 512"
	"1 1 4096"
	"4 4 64"
	"4 4 4096"
	"8 8 512"
	"16 1 4096"
	"1 16 64"
	"1 1 64 --nonblock"
)

for target in $targets; do
	path=$dev
	label=$([ "$mode" = Y ] && echo kq-excl || echo kq-all)
	if [ "$target" = fifo ]; then
		path=$fifo
		label=fifo
	fi
	for c in "${configs[@]}"; do
		read -r w r s extra <<<"$c"
		for rep in $(seq "$reps"); do
			echo "+ $label w=$w r=$r size=$s ${extra:-} rep=$rep"
			# shellcheck disable=SC2086
			"$bench" -d "$path" -w "$w" -r "$r" -s "$s" -t "$secs" ${extra:-} \
				--csv --label "$label" >> "$out/results.csv"
		done
	done
done

python3 "$root/bench/summarize.py" "$out/results.csv" > "$out/summary.md"
cat "$out/summary.md"
echo "results in $out"
