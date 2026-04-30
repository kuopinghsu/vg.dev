#!/usr/bin/env python3
"""Show pixel grid for a TGA image."""
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

bpx = rbpp // 4  # bytes per pixel

# Print all non-zero pixels in cmodel
print("Non-zero cmodel pixels (all):")
count = 0
for y in range(ch):
    for x in range(cw):
        idx = (y * cw + x) * (cbpp // 8)
        r, g, b, a = cpx[idx], cpx[idx+1], cpx[idx+2], cpx[idx+3]
        if r or g or b:
            ri_idx = (y * rw + x) * (rbpp // 8)
            rr, rg, rb, ra = rpx[ri_idx], rpx[ri_idx+1], rpx[ri_idx+2], rpx[ri_idx+3]
            print(f"  ({x:3d},{y:3d}) CM=({r:3d},{g:3d},{b:3d}) RI=({rr:3d},{rg:3d},{rb:3d})")
            count += 1
            if count > 100: 
                print(f"  ... (showing first 100, total non-zero: ?)")
                break
    else:
        continue
    break

if count <= 100:
    print(f"Total non-zero cmodel pixels: {count}")
