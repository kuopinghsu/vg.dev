#!/usr/bin/env python3
"""Show TGA pixel info and diffs."""
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
    with open(path, 'rb') as f:
        data = f.read()
    id_len   = data[0]
    img_type = data[2]
    w   = struct.unpack_from('<H', data, 12)[0]
    h   = struct.unpack_from('<H', data, 14)[0]
    bpp = data[16]  # byte 16 = pixel depth
    hdr = 18 + id_len
    if img_type in (10, 11):
        px = decode_rle(data, hdr, w*h, bpp//8)
    else:
        px = data[hdr:]
    return w, h, bpp, px

def show(label, path):
    w, h, bpp, px = read_tga(path)
    bpp_bytes = bpp // 8
    print(f"{label}: {w}x{h} bpp={bpp} ({len(px)} pixel bytes)")
    # Show first 8 pixels (raw bytes)
    for i in range(min(8, w*h)):
        chunk = px[i*bpp_bytes:(i+1)*bpp_bytes]
        print(f"  px[{i}] = {tuple(chunk)}")

if len(sys.argv) < 3:
    print("Usage: inspect_tga.py <ri.tga> <cmodel.tga>")
    sys.exit(1)

ri_path = sys.argv[1]
cm_path = sys.argv[2]

show("RI", ri_path)
show("CM", cm_path)

# Show diff
rw, rh, rbpp, rpx = read_tga(ri_path)
cw, ch, cbpp, cpx = read_tga(cm_path)

bpp_bytes = rbpp // 8
diff_count = 0
print("\nFirst 16 differing pixels:")
for i in range(min(rw*rh, cw*ch)):
    ri_px = rpx[i*bpp_bytes:(i+1)*bpp_bytes]
    cm_px = cpx[i*bpp_bytes:(i+1)*bpp_bytes]
    if ri_px != cm_px:
        x, y = i % rw, i // rw
        print(f"  ({x:3d},{y:3d}) RI={tuple(ri_px)} CM={tuple(cm_px)}")
        diff_count += 1
        if diff_count >= 16:
            break
total_diff = sum(1 for i in range(min(rw*rh, cw*ch))
                 if rpx[i*bpp_bytes:(i+1)*bpp_bytes] != cpx[i*bpp_bytes:(i+1)*bpp_bytes])
print(f"\nTotal differing pixels: {total_diff} / {min(rw*rh, cw*ch)}")
