#!/usr/bin/env python3
"""Check specific pixels in RI vs cmodel TGA."""
import sys, struct

def decode_rle(data, offset, n_pixels, bpp_bytes):
    out = bytearray()
    while len(out) < n_pixels * bpp_bytes:
        if offset >= len(data): break
        hdr = data[offset]; offset += 1
        count = (hdr & 0x7F) + 1
        if hdr & 0x80:
            pixel = data[offset:offset+bpp_bytes]; offset += bpp_bytes
            for _ in range(count): out.extend(pixel)
        else:
            raw = data[offset:offset+count*bpp_bytes]; offset += count*bpp_bytes
            out.extend(raw)
    return bytes(out[:n_pixels * bpp_bytes])

def read_tga(path):
    with open(path, 'rb') as f: data = f.read()
    id_len = data[0]; img_type = data[2]
    w = struct.unpack_from('<H', data, 12)[0]
    h = struct.unpack_from('<H', data, 14)[0]
    bpp = data[16]
    hdr = 18 + id_len
    px = decode_rle(data, hdr, w*h, bpp//8) if img_type in (10,11) else data[hdr:]
    return w, h, bpp, bytes(px)

ri_path = sys.argv[1]
cm_path = sys.argv[2]

rw, rh, rbpp, rpx = read_tga(ri_path)
cw, ch, cbpp, cpx = read_tga(cm_path)

rbpx = rbpp // 8
cbpx = cbpp // 8

# Count total matching vs mismatching pixels
match = 0
mismatch = 0
total_nonzero_ri = 0
total_nonzero_cm = 0

for y in range(rh):
    for x in range(rw):
        ri_idx = (y * rw + x) * rbpx
        cm_idx = (y * cw + x) * cbpx
        rr, rg, rb, ra = rpx[ri_idx], rpx[ri_idx+1], rpx[ri_idx+2], rpx[ri_idx+3]
        cr, cg, cb, ca = cpx[cm_idx], cpx[cm_idx+1], cpx[cm_idx+2], cpx[cm_idx+3]
        if rr or rg or rb: total_nonzero_ri += 1
        if cr or cg or cb: total_nonzero_cm += 1
        if (rr,rg,rb,ra) == (cr,cg,cb,ca): match += 1
        else: mismatch += 1

print(f"Total pixels: {rw*rh}")
print(f"Exact matches: {match}")
print(f"Mismatches: {mismatch}")
print(f"RI non-zero: {total_nonzero_ri}")
print(f"CM non-zero: {total_nonzero_cm}")

# Sample some specific interior pixels
print("\nSample pixels (interior):")
for y in [5, 10, 15, 20, 25, 30]:
    for x in [5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60]:
        ri_idx = (y * rw + x) * rbpx
        cm_idx = (y * cw + x) * cbpx
        rr, rg, rb = rpx[ri_idx], rpx[ri_idx+1], rpx[ri_idx+2]
        cr, cg, cb = cpx[cm_idx], cpx[cm_idx+1], cpx[cm_idx+2]
        if (rr, rg, rb) != (cr, cg, cb):
            print(f"  ({x:2d},{y:2d}) RI=({rr},{rg},{rb}) CM=({cr},{cg},{cb})")
