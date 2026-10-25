#!/usr/bin/env python3
"""Attribute KCSAN reports by the function that made each racing access.

A KCSAN report shows two accesses ("read to 0x... by task N on cpu M:" or
"... by interrupt ..."), each followed by a stack trace. The first frame of
each trace is the function that performed that access. A race "belongs to"
kernqueue only if one of those first frames is a kernqueue function; a
kernqueue frame deeper in a stack only means kernqueue called into the code
that raced (e.g. its mutex_lock() reaching osq_lock()).

Usage: kcsan_attribute.py dmesg-kcsan-*.log
Prints per-report access functions and a summary; exits 1 if any racing
access is in kernqueue.
"""
import re
import sys
from collections import Counter

ACCESS = re.compile(r"\] (read|write|read-write|watchpoint)[^:]* (to|of) .* by (task|interrupt)|"
                    r"\] race at unknown origin")
FRAME = re.compile(r"\]\s+([A-Za-z0-9_.]+)\+0x[0-9a-f]+/0x[0-9a-f]+(\s+\[([a-z0-9_]+)\])?")


def reports(lines):
    blk = []
    for line in lines:
        if "BUG: KCSAN:" in line:
            if blk:
                yield blk
            blk = [line]
        elif blk:
            blk.append(line)
    if blk:
        yield blk


def main() -> int:
    total, ours = 0, 0
    pairs = Counter()
    for path in sys.argv[1:]:
        for blk in reports(open(path, errors="replace")):
            total += 1
            accessors = []
            want_frame = False
            for line in blk[1:]:
                if ACCESS.search(line):
                    want_frame = True
                    continue
                if want_frame:
                    m = FRAME.search(line)
                    if m:
                        accessors.append((m.group(1), m.group(3) or "vmlinux"))
                        want_frame = False
            if any(mod == "kernqueue" for _, mod in accessors):
                ours += 1
            pairs[" / ".join(f"{fn}[{mod}]" for fn, mod in accessors)] += 1
    for pair, n in pairs.most_common():
        print(f"{n:5d}  {pair}")
    print(f"reports: {total}; racing access inside kernqueue: {ours}")
    return 1 if ours else 0


if __name__ == "__main__":
    sys.exit(main())
