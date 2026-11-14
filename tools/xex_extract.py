#!/usr/bin/env python3
"""Decrypt and decompress an Xbox 360 XEX2 into a flat PE image.

The output is the image as the console would see it at `image base` + RVA:
byte 0 is the PE header, so `default.image.bin` can be loaded at 0x82000000
and disassembled with RVA == file offset.

Supported: encryption none/normal, compression none/basic.  "normal"
compression (LZX) is not implemented.

AES-128-CBC is done through the `openssl` CLI so there is no Python
dependency.  Algorithm matches Xenia's xex_module.cc:

    session_key = AES-CBC-decrypt(retail_key, security->aes_key, iv=0)
    image      = AES-CBC-decrypt(session_key, body, iv=0)   # iv persists
                                                           # across blocks

Xbox 360 XEX2 fields are big-endian.
"""
import argparse
import hashlib
import struct
import subprocess
import sys

# The XEX2 retail key is not distributed with this project. Supply it yourself (32 hex digits) through the
# XEX_RETAIL_KEY environment variable or a file named by XEX_RETAIL_KEY_FILE.
def retail_key():
    import os
    text = os.environ.get("XEX_RETAIL_KEY", "")
    if not text and os.environ.get("XEX_RETAIL_KEY_FILE"):
        text = open(os.environ["XEX_RETAIL_KEY_FILE"]).read()
    text = text.strip()
    if len(text) != 32:
        sys.exit("set XEX_RETAIL_KEY (32 hex digits) or XEX_RETAIL_KEY_FILE to decrypt an encrypted XEX")
    return bytes.fromhex(text)

SECTOR = 0x1000

ENC = {0: "none", 1: "normal"}
CMP = {0: "none", 1: "basic", 2: "normal (LZX)"}


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def aes_cbc_decrypt(key, data):
    p = subprocess.run(
        ["openssl", "enc", "-aes-128-cbc", "-d", "-nopad", "-K", key.hex(),
         "-iv", "00" * 16],
        input=data, capture_output=True)
    if p.returncode != 0:
        sys.exit("openssl failed: " + p.stderr.decode(errors="replace"))
    return p.stdout


def parse(d):
    if d[:4] != b"XEX2":
        sys.exit(f"not an XEX2 file (magic {d[:4]!r})")
    header_size = u32(d, 0x8)
    sec_off = u32(d, 0x10)
    count = u32(d, 0x14)
    kv = {}
    for i in range(count):
        kv[u32(d, 0x18 + i * 8)] = u32(d, 0x1C + i * 8)
    ff = kv[0x000003FF]
    info_size = u32(d, ff)
    # Two u16 BE fields (not u8/u8 as some headers claim -- verified against
    # this image, whose encryption byte is impossible to read as a u8).
    enc = struct.unpack_from(">H", d, ff + 4)[0]
    cmp = struct.unpack_from(">H", d, ff + 6)[0]
    blocks = []
    if cmp == 1:
        off = ff + 8
        while off + 8 <= ff + info_size:
            ds, zs = struct.unpack_from(">II", d, off)
            if ds == 0 and zs == 0:
                break
            blocks.append((ds, zs))
            off += 8
    return {
        "header_size": header_size,
        "security": sec_off,
        "image_size": u32(d, sec_off + 4),
        "load_address": u32(d, sec_off + 0x110),
        "image_base": kv.get(0x00010201, 0),
        "entry": kv.get(0x00010100, 0),
        "info_size": info_size,
        "encryption": enc,
        "compression": cmp,
        "blocks": blocks,
        "aes_key": d[sec_off + 0x150:sec_off + 0x160],
    }


def extract(d):
    info = parse(d)
    body = d[info["header_size"]:]
    if info["encryption"] == 1:
        session = aes_cbc_decrypt(retail_key(), info["aes_key"])
        if len(session) != 16:
            sys.exit("could not derive session key")
        body = aes_cbc_decrypt(session, body)

    if info["compression"] == 0:
        image = body
    elif info["compression"] == 1:
        image = bytearray()
        pos = 0
        for data_size, zero_size in info["blocks"]:
            image += body[pos:pos + data_size]
            pos += data_size
            image += b"\x00" * zero_size
        image = bytes(image)
    else:
        sys.exit(f"unsupported compression: {CMP.get(info['compression'])}")

    if image[:2] != b"MZ":
        sys.exit("decrypted image does not start with MZ (wrong key/scheme?)")
    if len(image) != info["image_size"]:
        print(f"warning: image is {len(image)} bytes, header says "
              f"{info['image_size']}", file=sys.stderr)
    return info, image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xex")
    ap.add_argument("output", nargs="?", default="default.image.bin")
    args = ap.parse_args()

    d = open(args.xex, "rb").read()
    info, image = extract(d)
    open(args.output, "wb").write(image)

    print(f"encryption      {ENC.get(info['encryption'], info['encryption'])}")
    print(f"compression     {CMP.get(info['compression'], info['compression'])}")
    print(f"image base      0x{info['image_base']:08X}")
    print(f"entry point     0x{info['entry']:08X} "
          f"(RVA 0x{info['entry'] - info['image_base']:08X})")
    print(f"image size      0x{len(image):X} ({len(image):,} bytes)")
    print(f"sha256          {hashlib.sha256(image).hexdigest()}")
    print(f"written         {args.output}")


if __name__ == "__main__":
    main()
