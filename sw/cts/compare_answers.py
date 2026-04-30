#!/usr/bin/env python3
"""
Compare CTS answer files between RI and cmodel.
Usage: python3 compare_answers.py [config] [colorspace]
  config:     config id (default: 1)
  colorspace: sRGB_NONPRE|sRGB_PRE|lRGB_NONPRE|lRGB_PRE (default: sRGB_NONPRE)
"""
import os, sys, struct

BUILD = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'cts')
config = sys.argv[1] if len(sys.argv) > 1 else '1'
cs     = sys.argv[2] if len(sys.argv) > 2 else 'sRGB_NONPRE'

ri_dir  = os.path.join(BUILD, 'ri',     'answer', config, cs)
cm_dir  = os.path.join(BUILD, 'cmodel', 'answer', config, cs)

ri_files  = set(os.listdir(ri_dir))
cm_files  = set(os.listdir(cm_dir))
common    = ri_files & cm_files
only_ri   = ri_files - cm_files
only_cm   = cm_files - ri_files

print(f"RI answer files   : {len(ri_files)}")
print(f"cmodel answer files: {len(cm_files)}")
print(f"Common files      : {len(common)}")
print(f"Only in RI        : {len(only_ri)}")
print(f"Only in cmodel    : {len(only_cm)}")
print()

def tga_decode_rle(data, offset, n_pixels, bpp_bytes):
    """Decode RLE-compressed TGA pixel data into a bytearray of raw pixels."""
    out = bytearray()
    while len(out) < n_pixels * bpp_bytes:
        if offset >= len(data):
            break
        hdr = data[offset]; offset += 1
        count = (hdr & 0x7F) + 1
        if hdr & 0x80:  # RLE packet
            pixel = data[offset:offset + bpp_bytes]; offset += bpp_bytes
            for _ in range(count):
                out.extend(pixel)
        else:           # raw packet
            raw = data[offset:offset + count * bpp_bytes]; offset += count * bpp_bytes
            out.extend(raw)
    return bytes(out[:n_pixels * bpp_bytes])

def tga_pixels(path):
    """Read a .tga file and return (w, h, bpp, raw_pixel_bytes)."""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 18:
        return None
    id_len   = data[0]
    img_type = data[2]
    w = struct.unpack_from('<H', data, 12)[0]
    h = struct.unpack_from('<H', data, 14)[0]
    bpp = data[16]  # byte 16 = pixel depth
    bpp_bytes = max(1, bpp // 8)
    hdr_size = 18 + id_len
    raw = data[hdr_size:]
    if img_type in (10, 11):  # RLE compressed
        px = tga_decode_rle(data, hdr_size, w * h, bpp_bytes)
    else:                      # uncompressed
        px = raw
    return (w, h, bpp, px)

match = 0
mismatch = 0
mismatch_files = []
match_files = []

for fname in sorted(common):
    ri_path  = os.path.join(ri_dir,  fname)
    cm_path  = os.path.join(cm_dir, fname)

    if fname.endswith('.dat'):
        with open(ri_path, 'rb') as f: ri_data = f.read()
        with open(cm_path, 'rb') as f: cm_data = f.read()
        if ri_data == cm_data:
            match += 1
        else:
            mismatch += 1
            mismatch_files.append((fname, 'dat_diff'))
        continue

    if fname.endswith('.tga'):
        ri_tga = tga_pixels(ri_path)
        cm_tga = tga_pixels(cm_path)
        if ri_tga is None or cm_tga is None:
            mismatch += 1
            mismatch_files.append((fname, 'tga_parse_err'))
            continue
        rw, rh, rbpp, rpx = ri_tga
        cw, ch, cbpp, cpx = cm_tga
        if rw != cw or rh != ch or rbpp != cbpp:
            mismatch += 1
            mismatch_files.append((fname, f'tga_dim {rw}x{rh} vs {cw}x{ch} bpp={rbpp}/{cbpp}'))
            continue
        if rpx == cpx:
            match += 1
            match_files.append(fname)
        else:
            # Count differing bytes
            diff_bytes = sum(1 for a, b in zip(rpx, cpx) if a != b)
            mismatch += 1
            mismatch_files.append((fname, f'tga_pixel diff={diff_bytes}/{len(rpx)} bytes'))
        continue

    # Unknown extension: byte compare
    with open(ri_path, 'rb') as f: ri_data = f.read()
    with open(cm_path, 'rb') as f: cm_data = f.read()
    if ri_data == cm_data:
        match += 1
        match_files.append(fname)
    else:
        mismatch += 1
        mismatch_files.append((fname, 'binary_diff'))

total = match + mismatch
print(f"Exact matches  : {match} / {total}  ({100*match//total if total else 0}%)")
print(f"Mismatches     : {mismatch} / {total}")
print()

if mismatch_files:
    print(f"First 30 mismatches:")
    for f, reason in mismatch_files[:30]:
        print(f"  {f}: {reason}")
    if len(mismatch_files) > 30:
        print(f"  ... and {len(mismatch_files)-30} more")

if match_files:
    print(f"\nAll matching files ({len(match_files)}):")
    for f in match_files:
        print(f"  {f}")

if only_ri:
    print(f"\nMissing from cmodel (first 20):")
    for f in sorted(only_ri)[:20]:
        print(f"  {f}")
