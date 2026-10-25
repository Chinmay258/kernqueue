#!/usr/bin/env python3
"""Summarize kq_bench CSV output: median of each metric over repetitions.

Usage: summarize.py results.csv  (prints a Markdown table)
"""
import csv
import statistics
import sys
from collections import defaultdict


def main() -> int:
    rows = list(csv.DictReader(open(sys.argv[1], newline="")))
    groups = defaultdict(list)
    for row in rows:
        key = (row["label"], int(row["writers"]), int(row["readers"]),
               int(row["size"]), row["nonblock"] == "1")
        groups[key].append(row)

    def med(rs, col):
        return statistics.median(float(r[col]) for r in rs)

    print("| target | W x R | size | mode | n | read MB/s | read kops/s "
          "| write() p50/p99 us | read() p50/p99 us | e2e p50/p99 us "
          "| sleeps w/r |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for key in sorted(groups):
        label, w, r, size, nb = key
        rs = groups[key]
        us = lambda c: med(rs, c) / 1000.0  # noqa: E731
        sleeps = (f"{med(rs, 'write_waits'):.0f}/{med(rs, 'read_waits'):.0f}"
                  if label.startswith("kq") else "n/a")
        print(f"| {label} | {w}x{r} | {size} | {'nonblock' if nb else 'block'} "
              f"| {len(rs)} | {med(rs, 'read_MBps'):.1f} "
              f"| {med(rs, 'read_kops'):.1f} "
              f"| {us('w_p50_ns'):.1f} / {us('w_p99_ns'):.1f} "
              f"| {us('r_p50_ns'):.1f} / {us('r_p99_ns'):.1f} "
              f"| {us('e2e_p50_ns'):.1f} / {us('e2e_p99_ns'):.1f} "
              f"| {sleeps} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
