#!/usr/bin/env python3
"""
compare.py – pixel-wise comparison of two PPM images (RI vs cmodel).

Usage:
    python3 compare.py <ri.ppm> <cmodel.ppm> <test_name>

Exit code: 0 = PASS, 1 = FAIL or error.

A diff PPM (diff.ppm) is written next to the inputs showing amplified
per-channel absolute differences (×4, clamped to 255).

PASS criteria:
  • mean per-pixel max-channel difference < 20
  • fewer than 15 % of pixels have max-channel difference > 30
"""

import sys
import os
import struct


def read_ppm(path):
    with open(path, 'rb') as f:
        magic = f.readline().strip()
        if magic != b'P6':
            raise ValueError(f'{path}: not a P6 PPM file')
        # skip comment lines
        line = f.readline()
        while line.startswith(b'#'):
            line = f.readline()
        w, h = map(int, line.split())
        maxval = int(f.readline())
        raw = f.read()
    if len(raw) < w * h * 3:
        raise ValueError(f'{path}: truncated pixel data ({len(raw)} bytes, expected {w*h*3})')
    # unpack into list of (R,G,B) tuples
    pixels = list(struct.unpack_from(f'{w*h*3}B', raw))
    rgb = [(pixels[i*3], pixels[i*3+1], pixels[i*3+2]) for i in range(w * h)]
    return w, h, rgb


def write_diff_ppm(path, w, h, pa, pb):
    """Write an amplified-difference PPM for visual inspection."""
    with open(path, 'wb') as f:
        f.write(f'P6\n{w} {h}\n255\n'.encode())
        for a, b in zip(pa, pb):
            dr = min(255, abs(int(a[0]) - int(b[0])) * 4)
            dg = min(255, abs(int(a[1]) - int(b[1])) * 4)
            db = min(255, abs(int(a[2]) - int(b[2])) * 4)
            f.write(bytes([dr, dg, db]))


def compare(path_a, path_b, name):
    try:
        wa, ha, pa = read_ppm(path_a)
    except Exception as e:
        print(f'[ERROR] {name}: cannot read {path_a}: {e}')
        return False
    try:
        wb, hb, pb = read_ppm(path_b)
    except Exception as e:
        print(f'[ERROR] {name}: cannot read {path_b}: {e}')
        return False

    if wa != wb or ha != hb:
        print(f'[FAIL ] {name}: size mismatch  RI={wa}x{ha}  cmodel={wb}x{hb}')
        return False

    total     = wa * ha
    sum_diff  = 0
    high_diff = 0   # pixels where max-channel diff > 30
    max_diff  = 0

    for a, b in zip(pa, pb):
        d = max(abs(int(a[0]) - int(b[0])),
                abs(int(a[1]) - int(b[1])),
                abs(int(a[2]) - int(b[2])))
        sum_diff += d
        if d > max_diff:
            max_diff = d
        if d > 30:
            high_diff += 1

    mean_diff = sum_diff / total
    pct_high  = 100.0 * high_diff / total

    passed = (mean_diff < 20.0) and (pct_high < 15.0)
    status = 'PASS ' if passed else 'FAIL '

    print(f'[{status}] {name:<26s}  mean={mean_diff:5.1f}  '
          f'max={max_diff:3d}  high_diff={pct_high:5.1f}%')

    # Write diff image alongside ri.ppm
    diff_path = os.path.join(os.path.dirname(path_a), 'diff.ppm')
    write_diff_ppm(diff_path, wa, ha, pa, pb)

    return passed


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print('Usage: compare.py <ri.ppm> <cmodel.ppm> <test_name>')
        sys.exit(1)
    ok = compare(sys.argv[1], sys.argv[2], sys.argv[3])
    sys.exit(0 if ok else 1)
