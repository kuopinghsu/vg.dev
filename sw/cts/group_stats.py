#!/usr/bin/env python3
"""Group-by-group analysis of CTS comparison."""
import os, sys, struct, collections

BUILD = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'cts')
config = sys.argv[1] if len(sys.argv) > 1 else '1'
cs     = sys.argv[2] if len(sys.argv) > 2 else 'sRGB_NONPRE'

ri_dir  = os.path.join(BUILD, 'ri',     'answer', config, cs)
cm_dir  = os.path.join(BUILD, 'cmodel', 'answer', config, cs)

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

def tga_pixels(path):
    with open(path, 'rb') as f: data = f.read()
    if len(data) < 18: return None
    id_len = data[0]; img_type = data[2]
    w = struct.unpack_from('<H', data, 12)[0]; h = struct.unpack_from('<H', data, 14)[0]
    bpp = data[16]; hdr = 18 + id_len
    px = decode_rle(data, hdr, w*h, bpp//8) if img_type in (10,11) else data[hdr:]
    return bytes(px[:w*h*(bpp//8)])

ri_files = set(os.listdir(ri_dir))
cm_files = set(os.listdir(cm_dir))
common = ri_files & cm_files

groups = collections.defaultdict(lambda: {'match':0,'mismatch':0,'only_ri':0})

# Count by group
for f in ri_files - cm_files:
    grp = f[0]
    groups[grp]['only_ri'] += 1

for fname in sorted(common):
    grp = fname[0]
    ri_path = os.path.join(ri_dir, fname)
    cm_path = os.path.join(cm_dir, fname)
    if fname.endswith('.tga'):
        rpx = tga_pixels(ri_path)
        cpx = tga_pixels(cm_path)
        if rpx == cpx: groups[grp]['match'] += 1
        else: groups[grp]['mismatch'] += 1
    else:
        with open(ri_path,'rb') as f: rd = f.read()
        with open(cm_path,'rb') as f: cd = f.read()
        if rd == cd: groups[grp]['match'] += 1
        else: groups[grp]['mismatch'] += 1

print(f"Group | Match | Mismatch | Only-RI | Total")
print("-" * 45)
for g in sorted(groups.keys()):
    d = groups[g]
    total = d['match'] + d['mismatch'] + d['only_ri']
    pct = 100 * d['match'] // (d['match'] + d['mismatch']) if (d['match']+d['mismatch']) > 0 else 0
    print(f"  {g}   |  {d['match']:3d}  |   {d['mismatch']:3d}    |   {d['only_ri']:3d}   | {total:3d}  ({pct}%)")
