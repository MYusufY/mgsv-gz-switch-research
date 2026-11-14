#!/usr/bin/env python3
"""Find function entry points that ReXGlue's .pdata scan missed.

The Xbox 360 .pdata table does not describe every function. MSVC leaves large
holes in it, and those holes are almost always **thunk tables**: contiguous
runs of tiny forwarders

    mr   r3, r4
    mr   r4, r5
    b    real_function

plus 4-byte `b target` jump stubs. Because .pdata has no record for them, the
recompiler never emits a body, and any *indirect* call into one of them traps
with "invalid function" at run time.

Two independent signals decide which holes matter:

  1. big-endian pointers in .rdata/.data that land in .text but have no body
     (these are vtable slots);
  2. addresses built by `lis`+`addi`/`ori` and then loaded/branched to in code.

A gap that contains at least one such candidate is then split into individual
thunk boundaries: every 4-byte-aligned word inside the gap that is preceded by
zero padding or by an unconditional `b`/`ba` starts a new thunk. Without this
split a single candidate swallows the whole table and its neighbours still trap.

    python3 tools/find_gaps.py extracted/default.image.bin \
        port/generated/default/mgsv_gz_init.cpp [-v]

Prints a TOML `[functions]` body for port/overrides.toml. Feed the result
through tools/prune_gap_overrides.py: some of these addresses are switch
labels rather than thunk entries, and declaring them splits a `bctr` switch.
"""

import argparse
import bisect
import collections
import re
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

IMAGE_BASE = 0x82000000
MARKER = "# Entries discovered by tools/find_gaps.py"


def sections(img):
    e = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, e + 6)[0]
    optsz = struct.unpack_from("<H", img, e + 20)[0]
    out = {}
    for i in range(nsec):
        o = e + 24 + optsz + i * 40
        name = img[o:o + 8].rstrip(b"\0").decode()
        vsize, va = struct.unpack_from("<II", img, o + 8)
        raw = struct.unpack_from("<I", img, o + 20)[0]
        out[name] = (IMAGE_BASE + va, vsize, raw)
    return out


def known_functions(path):
    addrs = set()
    for line in open(path):
        m = re.match(r"\s*\{\s*(0x[0-9A-Fa-f]+),\s*\w+\s*\},", line)
        if m:
            addrs.add(int(m.group(1), 16))
    return addrs


def parse_entries(text):
    return {int(m.group(1), 16)
            for m in re.finditer(r'"(0x[0-9A-Fa-f]+)"\s*=\s*\{', text)}


def override_entries(path):
    """Addresses declared by a previous run of this tool.

    They are in the generated map only because we asked for them, so treating
    them as "already known" would hide the thunks they currently swallow.
    Subtract them to recover the recompiler's own baseline discovery.
    """
    if not path:
        return set()
    try:
        return parse_entries(open(path).read())
    except FileNotFoundError:
        return set()


def hand_entries(path):
    """Hand-written boundaries (everything above the generated marker).

    They stay authoritative: a candidate that covers one must stop there.
    """
    if not path:
        return set()
    try:
        return parse_entries(open(path).read().split(MARKER)[0])
    except FileNotFoundError:
        return set()


def pdata_functions(img, secs, text_lo, text_hi):
    """Parse .pdata. The table is big-endian even in the LE image container."""
    _, size, raw = secs[".pdata"]
    out = []
    for off in range(0, size, 8):
        begin, info = struct.unpack_from(">II", img, raw + off)
        if begin == 0:
            continue
        length = ((info >> 8) & 0x3FFFFF) * 4
        if text_lo <= begin < text_hi:
            out.append((begin, length))
    out.sort()
    return out


def u32(img, addr):
    return struct.unpack_from(">I", img, addr - IMAGE_BASE)[0]


def is_unconditional_branch(word):
    # opcode 18 = b/ba; bit 0 = LK, so bl/bla do not terminate a thunk.
    return (word >> 26) == 18 and (word & 1) == 0


def thunk_starts(img, start, length, text_lo):
    """Boundaries of the tiny forwarders packed into a .pdata gap."""
    out = []
    for addr in range(start, start + length, 4):
        if u32(img, addr) == 0:
            continue
        prev = u32(img, addr - 4) if addr - 4 >= text_lo else 0
        if prev == 0 or is_unconditional_branch(prev):
            out.append(addr)
    return out


def data_pointer_hits(img, secs, text_lo, text_hi, known, covered):
    hits = set()
    for name in (".rdata", ".data"):
        _, size, raw = secs[name]
        for off in range(0, size - 3, 4):
            v = struct.unpack_from(">I", img, raw + off)[0]
            if text_lo <= v < text_hi and v not in known and not (v & 3):
                if not covered(v):
                    hits.add(v)
    return hits


def code_composed_hits(img, secs, text_lo, text_hi, known, covered):
    """`lis`+`addi`/`ori` address construction, tracked per register."""
    _, size, raw = secs[".text"]
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    md.detail = True
    md.skipdata = True  # .text has zero-filled alignment padding

    hits = set()
    reg = {}
    for ins in md.disasm(img[raw:raw + size], text_lo):
        if ins.id == 0:  # skipdata pseudo-instruction
            reg.clear()
            continue
        m, ops = ins.mnemonic, ins.operands

        if m == "lis" and len(ops) == 2:
            reg[ops[0].reg] = (ops[1].imm << 16) & 0xFFFFFFFF
            continue

        if m in ("addi", "addis", "ori", "oris") and len(ops) == 3:
            base = reg.get(ops[1].reg)
            if base is None:
                reg.pop(ops[0].reg, None)
                continue
            imm = ops[2].imm & 0xFFFFFFFF
            if m in ("addi", "addis"):
                value = (base + imm) & 0xFFFFFFFF
            else:
                value = base | imm
            reg[ops[0].reg] = value
            if m in ("addi", "ori") and text_lo <= value < text_hi:
                if not (value & 3) and value not in known and not covered(value):
                    hits.add(value)
            continue

        if m in ("lwz", "lwzu") and len(ops) == 3:
            base = reg.get(ops[1].reg)
            if base is not None:
                addr = (base + ops[2].imm) & 0xFFFFFFFF
                try:
                    v = u32(img, addr)
                except struct.error:
                    v = None
                if v is not None:
                    reg[ops[0].reg] = v
                    if text_lo <= v < text_hi and v not in known and not covered(v):
                        hits.add(v)
                    continue
            reg.pop(ops[0].reg, None)
            continue

        if ops and ops[0].type == 1:  # any other write clobbers the register
            reg.pop(ops[0].reg, None)
        if m.startswith("b"):
            reg.clear()
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("init_cpp", help="generated mgsv_gz_init.cpp (function map)")
    ap.add_argument("--overrides", default="port/overrides.toml",
                    help="existing overrides file; its entries are treated as "
                         "not yet discovered (default: port/overrides.toml)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    img = open(args.image, "rb").read()
    secs = sections(img)
    known = known_functions(args.init_cpp)
    previous = override_entries(args.overrides)
    known -= previous
    if previous:
        print(f"ignoring {len(previous)} previous override entries", file=sys.stderr)
    text_lo, text_size, _ = secs[".text"]
    text_hi = text_lo + text_size

    funcs = pdata_functions(img, secs, text_lo, text_hi)
    starts = [f[0] for f in funcs]

    def covered(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and addr < funcs[i][0] + funcs[i][1]

    gaps = []
    prev = text_lo
    for begin, length in funcs:
        if begin > prev:
            gaps.append((prev, begin - prev))
        prev = max(prev, begin + length)
    if prev < text_hi:
        gaps.append((prev, text_hi - prev))

    pointers = data_pointer_hits(img, secs, text_lo, text_hi, known, covered)
    composed = code_composed_hits(img, secs, text_lo, text_hi, known, covered)
    candidates = pointers | composed

    # Split every gap that holds a candidate into its individual thunks.
    extras = collections.Counter()
    for gap_start, gap_len in gaps:
        if not any(gap_start <= c < gap_start + gap_len for c in candidates):
            continue
        for t in thunk_starts(img, gap_start, gap_len, text_lo):
            if t not in known and not covered(t):
                extras[t] += 1

    entries = candidates | set(extras)
    bound_set = set(starts) | entries | hand_entries(args.overrides)
    bounds = sorted(bound_set)

    def next_boundary(addr):
        i = bisect.bisect_right(bounds, addr)
        return bounds[i] if i < len(bounds) else text_hi

    rows = [(a, next_boundary(a) - a) for a in sorted(entries)]

    print(f"known functions       : {len(known)}", file=sys.stderr)
    print(f"pdata entries in .text: {len(funcs)}", file=sys.stderr)
    print(f"gaps                  : {len(gaps)}", file=sys.stderr)
    print(f"vtable pointer hits   : {len(pointers)}", file=sys.stderr)
    print(f"code-composed hits    : {len(composed)}", file=sys.stderr)
    print(f"thunk-split extras    : {len(set(extras) - candidates)}", file=sys.stderr)
    print(f"total entries         : {len(rows)}", file=sys.stderr)
    if args.verbose:
        for addr, size in rows:
            if size > 0x200:
                print(f"  warning: 0x{addr:08X} size 0x{size:X}", file=sys.stderr)

    print(MARKER)
    for addr, size in rows:
        print(f'"0x{addr:08X}" = {{ size = 0x{size:X} }}')
    return 0


if __name__ == "__main__":
    sys.exit(main())
