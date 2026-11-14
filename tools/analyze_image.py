#!/usr/bin/env python3
"""Analyze the decrypted Xbox 360 PE image of Metal Gear Solid V: Ground Zeroes.

Reports PE section layout, .pdata function count, and a rough instruction mix
for the .text section (integer / floating point / VMX-AltiVec / branch).

Requires capstone:  pip install capstone
"""
import argparse
import collections
import struct
import sys


def read_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    so = opt + struct.unpack_from("<H", data, pe + 20)[0]
    sections = {}
    for i in range(nsec):
        name = data[so + i * 40:so + i * 40 + 8].rstrip(b"\0").decode("latin1")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, so + i * 40 + 8)
        sections[name] = {
            "va": va,
            "vsize": vsize,
            "rawptr": rawptr,
            "rawsize": rawsize,
        }
    return pe, image_base, sections


def classify(mnemonic):
    m = mnemonic.split(".")[0].lower()
    if m.startswith("v"):
        return "vmx"
    if m.startswith("f"):
        return "fp"
    if m in ("bpermd", "mulld", "divd", "cmpd", "cmpld", "sradi", "sld", "srd"):
        return "int"
    if m.startswith("b"):
        return "branch"
    return "int"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="?", default="extracted/default.image.bin")
    ap.add_argument("--limit", type=int, default=0, help="only scan N bytes of .text")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    pe, image_base, sections = read_sections(data)

    print(f"image base      0x{image_base:08X}")
    print(f"PE header       0x{pe:X}")
    print(f"file size       {len(data):,} bytes")
    print()
    print(f"{'section':10s} {'RVA':>10s} {'vsize':>10s} {'file off':>10s} {'rawsize':>10s}")
    for name, s in sections.items():
        print(
            f"{name:10s} {s['va']:#010x} {s['vsize']:#010x} "
            f"{s['rawptr']:#010x} {s['rawsize']:#010x}"
        )
    print()

    if ".pdata" in sections:
        pd = sections[".pdata"]
        print(f".pdata functions {pd['vsize'] // 8:,}")
        print()

    try:
        import capstone
    except ImportError:
        print("capstone not installed; skipping instruction mix", file=sys.stderr)
        return 1

    # XEX images are flat: image base + RVA == file offset. Section raw
    # pointers in the PE header are not reliable here (the .text raw pointer is
    # 0x400 lower than its RVA), so index by RVA.
    text = sections[".text"]
    code = data[text["va"]:text["va"] + text["vsize"]]
    if args.limit:
        code = code[: args.limit]

    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    counts = collections.Counter()
    total = 0
    off = 0
    chunk = 1 << 16
    while off < len(code):
        start = off
        for ins in md.disasm(code[off:off + chunk], text["va"] + off):
            total += 1
            counts[classify(ins.mnemonic)] += 1
            counts["_" + ins.mnemonic.split(".")[0].lower()] += 1
            off = ins.address - text["va"] + ins.size
        if off == start:
            # Capstone cannot decode this word. Xenon VMX128 shares primary
            # opcodes 4/5/6 with AltiVec; all-zero words are section padding.
            word = struct.unpack_from(">I", code, off)[0]
            opcode = word >> 26
            total += 1
            if opcode in (4, 5, 6):
                counts["vmx"] += 1
            elif word == 0:
                counts["padding"] += 1
            else:
                counts["undecodable"] += 1
            off += 4
        if off >= len(code):
            break

    print(f"instructions     {total:,}")
    print(f"bytes/insn       {len(code) / total:.2f}")
    print()
    for kind in ("int", "fp", "vmx", "branch", "padding", "undecodable"):
        n = counts[kind]
        print(f"{kind:10s} {n:12,} {100.0 * n / total:6.2f}%")
    print()
    print("top 25 mnemonics:")
    for m, n in counts.most_common():
        if m.startswith("_"):
            print(f"  {m[1:]:12s} {n:10,} {100.0 * n / total:5.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
