#!/usr/bin/env bash
# Same-session A/B of the two wake-up modes (exclusive_wake=1 vs 0) on one
# workload. The modes alternate within each repetition, so slow drift in VM
# performance does not favour either side. Each step reloads the module
# (needs sudo; asks once) and runs an integrity stress run plus a benchmark.
#
#   scripts/ab_wake.sh [capacity] [writers] [readers] [size] [reps]
#   default:            4096       4         4         4096   3
set -uo pipefail

cap=${1:-4096}
w=${2:-4}
r=${3:-4}
size=${4:-4096}
reps=${5:-3}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out="build/logs/ab-$(date +%Y%m%d-%H%M%S)-cap$cap-${w}x$r-$size"
mkdir -p "$out"

sudo -v || exit 1
cleanup() { grep -q '^kernqueue ' /proc/modules && sudo rmmod kernqueue; }
trap cleanup EXIT

scripts/env_info.sh > "$out/environment.txt"
build/kq_bench --header > "$out/bench.csv"
status=0
for rep in $(seq "$reps"); do
	for mode in 1 0; do
		label=$([ "$mode" = 1 ] && echo kq-excl || echo kq-all)
		echo "+ rep $rep, $label"
		scripts/load_module.sh capacity="$cap" exclusive_wake="$mode" >/dev/null || exit 1
		if ! build/test_stress -q -w "$w" -r "$r" -n 50000 -s "$size" |
			sed "s/^/$label: /" | tee -a "$out/stress.log"; then
			status=1
		fi
		build/kq_bench -w "$w" -r "$r" -s "$size" -t 5 --csv --label "$label" >> "$out/bench.csv" || status=1
		scripts/unload_module.sh >/dev/null
	done
done
python3 bench/summarize.py "$out/bench.csv" | tee "$out/summary.md"
echo "A/B results in $out"
exit "$status"
