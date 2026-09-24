#!/usr/bin/env python3
"""Diff two MSDRV_REGLOG dumps; report match rate and first divergences."""
import sys

TIMER = {("OPN", "24"), ("OPN", "25"), ("OPN", "26"), ("OPN", "27"),
         ("OPNA", "24"), ("OPNA", "25"), ("OPNA", "26"), ("OPNA", "27")}

def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) < 5:
                continue
            tick, chip, port, reg, val = p[0], p[1], int(p[2]), p[3].upper(), p[4].upper()
            if (chip, reg) in TIMER:
                continue
            rows.append((int(tick), chip, port, reg, val))
    return rows

def main():
    if len(sys.argv) < 3:
        print("usage: msdrv_regdiff.py native.reg reference.reg")
        return 1
    a, b = load(sys.argv[1]), load(sys.argv[2])
    print(f"native={len(a)} writes  ref={len(b)} writes (timers stripped)")
    n = min(len(a), len(b))
    mismatches = 0
    first = {}
    for i in range(n):
        if a[i][1:] != b[i][1:]:
            key = (a[i][1], a[i][2], a[i][3])
            if key not in first:
                first[key] = (i, a[i], b[i])
            mismatches += 1
    matched = n - mismatches
    pct = 100.0 * matched / n if n else 0.0
    print(f"index-aligned match: {matched}/{n} = {pct:.1f}%")
    for k, v in list(first.items())[:15]:
        i, na, re = v
        print(f"  first @{i}: native {na} vs ref {re}")
    if len(a) != len(b):
        print(f"length delta: native-ref = {len(a)-len(b)}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
