#!/usr/bin/env python3
"""Diff two MSDRV_REGLOG dumps; report first divergence per chip/port/reg."""
import sys
from collections import defaultdict

def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line=line.strip()
            if not line or line.startswith('#'): continue
            parts=line.split()
            if len(parts) < 5: continue
            tick,chip,port,reg,val = parts[0],parts[1],int(parts[2]),parts[3],parts[4]
            rows.append((int(tick), chip, port, reg.upper(), val.upper()))
    return rows

def main():
    if len(sys.argv) < 3:
        print('usage: msdrv_regdiff.py native.reg reference.reg'); return 1
    a,b = load(sys.argv[1]), load(sys.argv[2])
    print(f'native={len(a)} writes  ref={len(b)} writes')
    # Align by index (same song → similar order). Also report first mismatch per key.
    n = min(len(a), len(b))
    first = {}
    mismatches = 0
    for i in range(n):
        if a[i][1:] != b[i][1:]:  # ignore tick skew
            key = (a[i][1], a[i][2], a[i][3])
            if key not in first:
                first[key] = (i, a[i], b[i])
            mismatches += 1
    print(f'index-aligned mismatches: {mismatches}/{n}')
    for k,v in list(first.items())[:30]:
        i,na,re = v
        print(f'  first @{i}: native {na} vs ref {re}')
    if len(a) != len(b):
        print(f'length delta: native-ref = {len(a)-len(b)}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
