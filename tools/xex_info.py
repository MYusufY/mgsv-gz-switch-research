#!/usr/bin/env python3
"""Parse an Xbox 360 XEX2 executable header (no decompression).

Layout follows xenia's src/xenia/kernel/util/xex2_info.h.
"""
import struct
import sys

KEYS = {
    0x000002FF: "ResourceInfo",
    0x000003FF: "FileFormatInfo",
    0x000005FF: "DeltaPatchDescriptor",
    0x000080FF: "BoundingPath",
    0x00008105: "DeviceID",
    0x00010001: "OriginalBaseAddress",
    0x00010100: "EntryPoint",
    0x00010201: "ImageBaseAddress",
    0x000103FF: "ImportLibraries",
    0x00018002: "ChecksumTimestamp",
    0x000183FF: "OriginalPEName",
    0x000200FF: "StaticLibraries",
    0x00020104: "TLSInfo",
    0x00020200: "DefaultStackSize",
    0x00020301: "DefaultFilesystemCacheSize",
    0x00020401: "DefaultHeapSize",
    0x00030000: "SystemFlags",
    0x00030100: "SystemFlags2",
    0x00040006: "ExecutionInfo",
    0x00040201: "TitleWorkspaceSize",
    0x00040310: "GameRatings",
    0x00040404: "LANKey",
    0x000405FF: "Xbox360Logo",
}

ENC = {0: "none", 1: "normal"}
CMP = {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def ver(v):
    return f"{(v >> 28) & 0xF}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def main(path):
    d = open(path, "rb").read()
    if d[:4] != b"XEX2":
        sys.exit(f"not XEX2: {d[:4]!r}")

    header_count = u32(d, 0x14)
    sec_offset = u32(d, 0x10)
    kv = {}
    print(f"magic            XEX2")
    print(f"module flags     0x{u32(d, 4):08x}")
    print(f"header size      0x{u32(d, 8):x}")
    print(f"security offset  0x{sec_offset:x}")
    for i in range(header_count):
        k, v = u32(d, 0x18 + i * 8), u32(d, 0x1C + i * 8)
        kv[k] = v
    print("\nheader keys:")
    for k, v in kv.items():
        print(f"  0x{k:08x} {KEYS.get(k, '?'):<28} 0x{v:08x}")

    image_base = kv.get(0x00010201)
    entry = kv.get(0x00010100)
    print(f"\nimage base       0x{image_base:08x}")
    print(f"entry point      0x{entry:08x} (RVA 0x{entry - image_base:08x})")
    if sec_offset:
        print(f"image size       0x{u32(d, sec_offset + 4):x}")
    if 0x00020200 in kv:
        print(f"default stack    0x{kv[0x00020200]:x}")
    if 0x00020401 in kv:
        print(f"default heap     0x{kv[0x00020401]:x}")

    # execution info
    ei = kv.get(0x00040006)
    if ei:
        media_id, v, bv, title_id = struct.unpack_from(">IIII", d, ei)
        platform, table, disc, count = d[ei + 0x10:ei + 0x14]
        print(f"\ntitle id         {title_id:08X}")
        print(f"media id         {media_id:08X}")
        print(f"version          {ver(v)} (base {ver(bv)})")
        print(f"platform/table   {platform}/{table}  disc {disc}/{count}")

    # file format / compression
    ff = kv.get(0x000003FF)
    if ff:
        info_size = u32(d, ff)
        enc = u16(d, ff + 4)
        cmp = u16(d, ff + 6)
        print(f"\nfile format      info_size={info_size} encryption={ENC.get(enc, enc)} "
              f"compression={CMP.get(cmp, cmp)}")
        if cmp == 1:
            off = ff + 8
            print("  basic-compression blocks (data_size, zero_size):")
            while off + 8 <= ff + info_size:
                ds, zs = struct.unpack_from(">II", d, off)
                if ds == 0 and zs == 0:
                    break
                print(f"    0x{ds:08x} 0x{zs:08x}")
                off += 8
        elif cmp == 2:
            window = u32(d, ff + 8)
            block_size = u32(d, ff + 12)
            print(f"  LZX window=0x{window:x} first_block=0x{block_size:x}")

    # static libraries (linked-in code)
    sl = kv.get(0x000200FF)
    if sl:
        size = u32(d, sl)
        count = (size - 4) // 0x10
        print(f"\nstatic libraries ({count}):")
        for i in range(count):
            o = sl + 4 + i * 0x10
            name = d[o:o + 8].split(b"\x00")[0].decode("latin-1")
            maj, minr, build = struct.unpack_from(">HHH", d, o + 8)
            print(f"  {name:<10} {maj}.{minr}.{build}")

    # import libraries (xboxkrnl / xam)
    imp = kv.get(0x000103FF)
    if imp:
        total = u32(d, imp)
        st_size = u32(d, imp + 4)
        st_count = u32(d, imp + 8)
        names_blob = d[imp + 0xC:imp + 0xC + st_size]
        names = [n.decode("latin-1") for n in names_blob.split(b"\x00") if n]
        print(f"\nimport string table ({st_count}): {', '.join(names)}")
        rec = imp + 0xC + st_size
        end = imp + total
        print("import libraries:")
        while rec + 0x28 <= end:
            lsize = u32(d, rec)
            if lsize < 0x28:
                break
            lib_id = u32(d, rec + 0x18)
            lver = u32(d, rec + 0x1C)
            lmin = u32(d, rec + 0x20)
            name_index = u16(d, rec + 0x24)
            ncount = u16(d, rec + 0x26)
            name = names[name_index] if name_index < len(names) else f"?{name_index}"
            print(f"  {name:<16} id=0x{lib_id:08x} version={ver(lver)} "
                  f"min={ver(lmin)} imports={ncount}")
            rec += lsize


if __name__ == "__main__":
    main(sys.argv[1])
