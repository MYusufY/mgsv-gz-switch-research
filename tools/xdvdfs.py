#!/usr/bin/env python3
"""
XDVDFS (Xbox / Xbox 360 DVD filesystem) reader.

Notes discovered while analysing this disc:
  * The volume descriptor lives at partition_base + 0x10000.
  * Xbox 360 XDVDFS stores the volume-descriptor fields AND directory
    entry fields in LITTLE endian (original Xbox used big endian).
  * For redump-style full-disc images the game partition does not start at
    byte 0; this script probes the four well-known Xbox/XGD partition
    offsets before giving up.

Usage:
  python3 tools/xdvdfs.py list   <image.iso>
  python3 tools/xdvdfs.py manfiest <image.iso> -o manifest.tsv
  python3 tools/xdvdfs.py extract <image.iso> <path-in-image> -o out/
  python3 tools/xdvdfs.py extract-all <image.iso> -o out/
"""
import argparse
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# candidate partition base offsets for full-disc (redump) images
CANDIDATE_BASES = [0x0, 0x0FD90000, 0x02080000, 0x18300000]
ATTR_DIRECTORY = 0x10


class Entry:
    __slots__ = ("path", "sector", "size", "attr", "offset")

    def __init__(self, path, sector, size, attr, offset):
        self.path = path
        self.sector = sector
        self.size = size
        self.attr = attr
        self.offset = offset

    @property
    def is_dir(self):
        return bool(self.attr & ATTR_DIRECTORY)


class Xdvdfs:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.filesize = os.path.getsize(path)
        self.base = self._find_base()
        self.root_sector, self.root_size = self._read_descriptor()

    def _find_base(self):
        for base in CANDIDATE_BASES:
            off = base + 0x10000
            if off + 0x800 > self.filesize:
                continue
            self.f.seek(off)
            if self.f.read(20) == MAGIC:
                self.f.seek(off + 0x7EC)
                if self.f.read(20) == MAGIC:
                    return base
        raise RuntimeError("not an XDVDFS image (no volume descriptor found)")

    def _read_descriptor(self):
        self.f.seek(self.base + 0x10000)
        d = self.f.read(0x800)
        root_sector, root_size = struct.unpack_from("<II", d, 0x14)
        return root_sector, root_size

    def _walk_table(self, sector, size, prefix, out):
        table_off = self.base + sector * SECTOR
        self.f.seek(table_off)
        table = self.f.read(size)
        seen = set()
        stack = [(0, 0)]
        while stack:
            rel, depth = stack.pop()
            if rel in seen or rel + 14 > len(table):
                continue
            seen.add(rel)
            left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", table, rel)
            if rel + 14 + nlen > len(table):
                continue
            raw = table[rel + 14:rel + 14 + nlen]
            if not raw:
                continue
            name = raw.decode("latin-1")
            path = prefix + name
            entry = Entry(path, start, fsize, attr, self.base + start * SECTOR)
            out.append(entry)
            if entry.is_dir and start and fsize:
                self._walk_table(start, fsize, path + "/", out)
            # tree links are in DWORD units relative to the table start
            if right:
                stack.append((right * 4, depth))
            if left:
                stack.append((left * 4, depth + 1))

    def entries(self):
        out = []
        self._walk_table(self.root_sector, self.root_size, "/", out)
        return out

    def _copy_range(self, offset, size, dst):
        self.f.seek(offset)
        remaining = size
        with open(dst, "wb") as o:
            while remaining:
                chunk = self.f.read(min(1024 * 1024, remaining))
                if not chunk:
                    raise IOError("unexpected EOF")
                o.write(chunk)
                remaining -= len(chunk)

    def extract_all(self, outdir, skip_system_update=True):
        entries = self.entries()
        for e in entries:
            rel = e.path.lstrip("/")
            if skip_system_update and rel.startswith("$SystemUpdate"):
                continue
            dst = os.path.join(outdir, rel)
            if e.is_dir:
                os.makedirs(dst, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
                self._copy_range(e.offset, e.size, dst)
                print(f"  {rel} ({e.size} bytes)")

    def extract_one(self, img_path, outdir):
        target = "/" + img_path.lstrip("/")
        for e in self.entries():
            if e.path == target and not e.is_dir:
                dst = os.path.join(outdir, os.path.basename(e.path))
                os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
                self._copy_range(e.offset, e.size, dst)
                print(f"{e.path} -> {dst} ({e.size} bytes, disc offset 0x{e.offset:x})")
                return
        raise SystemExit(f"not found: {img_path}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("list", "manifest", "extract", "extract-all"):
        p = sub.add_parser(name)
        p.add_argument("image")
        if name in ("manifest", "extract", "extract-all"):
            p.add_argument("-o", "--outdir", required=True)
        if name == "extract":
            p.add_argument("path")
    args = ap.parse_args()

    x = Xdvdfs(args.image)
    print(f"partition base: 0x{x.base:x}  root sector: {x.root_sector} "
          f"({x.root_size} bytes)", file=sys.stderr)
    entries = x.entries()
    if args.cmd == "list":
        for e in entries:
            print(("d" if e.is_dir else "f") + f" {e.size:>12} {e.path}")
    elif args.cmd == "manifest":
        with open(os.path.join(args.outdir, "manifest.tsv"), "w") as fh:
            fh.write("type\tbytes\tsector\tdisc_offset\tpath\n")
            for e in entries:
                fh.write(f"{'dir' if e.is_dir else 'file'}\t{e.size}\t{e.sector}\t"
                         f"0x{e.offset:x}\t{e.path}\n")
        print(f"wrote {len(entries)} entries")
    elif args.cmd == "extract":
        x.extract_one(args.path, args.outdir)
    elif args.cmd == "extract-all":
        x.extract_all(args.outdir)


if __name__ == "__main__":
    main()
