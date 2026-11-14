#!/usr/bin/env python3
"""Build library containers for vertex-shader variants the ring loads but the library cannot identify.

Direct3D patches a vertex shader's fetch instructions for each vertex declaration (format, stride, offset, full/mini
fetch, destination register). The game-side dump only holds the template; the ring loads the patched program
(MGSV_RING_DUMP_DIR dumps it). For each ring program: find the template container (same length, every differing word
inside a vertex fetch), remap each declared element to the patched fetch that writes the same register and components,
and write template header + remapped element table + ring microcode as a new container.

  repair_vs_variants.py <ring dump dir> <container dir> [<container dir> ...] --out <dir> [--dry]
"""
import struct, sys, os, glob, argparse

def words(b): return [struct.unpack('>I', b[i:i + 4])[0] for i in range(0, len(b) - 3, 4)]

def fetches(w):
    """(instruction index) of every vertex fetch, in control-flow order."""
    out = []; n = len(w) // 3; end = n
    for pair in range(n):
        if pair >= end: break
        for c in (w[pair*3] | ((w[pair*3+1] & 0xFFFF) << 32), (w[pair*3+1] >> 16) | (w[pair*3+2] << 16)):
            op = (c >> 44) & 0xF
            if (1 <= op <= 6) or op in (13, 14):
                a = c & 0xFFF; k = (c >> 12) & 7; seq = (c >> 16) & 0xFFF
                if a: end = min(end, a)
                for i in range(k):
                    idx = a + i
                    if (seq >> (i * 2)) & 1 and idx * 3 + 2 < len(w) and (w[idx*3] & 0x1F) == 0:
                        out.append(idx)
    return out

def dst(w, i):
    d0, d1 = w[i*3], w[i*3+1]
    reg = (d0 >> 12) & 0x3F
    swz = d1 & 0xFFF
    comps = tuple(((swz >> (3*c)) & 7) for c in range(4))
    written = tuple(c for c in range(4) if comps[c] < 4)  # 0..3 = a fetched component; 4/5 = constant, 7 = keep
    return reg, written

def parse(path):
    d = bytearray(open(path, 'rb').read())
    if len(d) < 32 or (struct.unpack('>I', d[:4])[0] & 0xFFFFFF00) != 0x102A1100 or not (d[3] & 1): return None
    vsz = struct.unpack('>I', d[4:8])[0]; so = struct.unpack('>I', d[24:28])[0]
    po, size = struct.unpack('>II', d[so:so+8])
    prev, count = struct.unpack('>II', d[so+24:so+32])
    table = so + 36 + prev * 4
    elements = [(table + 4*i, struct.unpack('>I', d[table+4*i:table+4*i+4])[0]) for i in range(count)]
    return dict(data=d, vsz=vsz, po=po, size=size, elements=elements, code=words(bytes(d[vsz+po:vsz+po+size])))

ap = argparse.ArgumentParser()
ap.add_argument('ring'); ap.add_argument('dirs', nargs='+'); ap.add_argument('--out', required=True); ap.add_argument('--dry', action='store_true')
a = ap.parse_args()
templates = {}
for d in a.dirs:
    for p in glob.glob(os.path.join(d, 'vs_*.bin')):
        t = parse(p)
        if t and t['elements']: templates.setdefault(len(t['code']), []).append((p, t))
os.makedirs(a.out, exist_ok=True)
import hashlib
made = 0
for rp in sorted(glob.glob(os.path.join(a.ring, 'vs_ring_*.bin'))):
    rw = words(open(rp, 'rb').read())
    rf = fetches(rw); rfs = set(rf)
    best = None
    for p, t in templates.get(len(rw), []):
        tf = set(fetches(t['code']))
        diff = [i for i in range(len(rw)) if rw[i] != t['code'][i]]
        if any((i // 3) not in (tf | rfs) for i in diff): continue
        if best is None or len(diff) < best[0]: best = (len(diff), p, t)
    name = os.path.basename(rp)[:24]
    if not best: print(name, 'no template'); continue
    _, tp, t = best
    # declared element -> (reg, written) in the template -> the patched fetch with the same destination
    by_dst = {}
    for i in rf: by_dst.setdefault(dst(rw, i), []).append(i)
    new = bytearray(t['data']); ok = True; used = set()
    for off, value in t['elements']:
        ins = value & 0xFFF
        key = dst(t['code'], ins)
        cands = [i for i in by_dst.get(key, []) if i not in used]
        if not cands:  # same register, any written components
            cands = [i for k, v in by_dst.items() if k[0] == key[0] for i in v if i not in used]
        if not cands: ok = False; print(name, 'element at', ins, key, 'has no patched fetch'); break
        used.add(cands[0])
        struct.pack_into('>I', new, off, (value & ~0xFFF) | cands[0])
    unmapped = [i for i in rf if i not in used]
    if not ok: continue
    code = b''.join(struct.pack('>I', x) for x in rw)
    new[t['vsz']+t['po']:t['vsz']+t['po']+t['size']] = code
    h = hashlib.sha1(new).hexdigest()[:16]
    out = os.path.join(a.out, f'vs_{h}.bin')
    print(name, 'template', os.path.basename(tp)[:19], 'diff', best[0], 'unmapped fetches', len(unmapped), '->', os.path.basename(out))
    if not a.dry: open(out, 'wb').write(new); made += 1
print('written', made)
