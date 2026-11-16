#!/usr/bin/env python3
"""Reference zdx encoder/decoder used by host tests and the offline preview.

The on-device decoder lives in main/reader_zdx.c; this module produces the exact
byte layout it expects and is the oracle the host tests compare against. The
transfer web page uses pako with the same windowBits=12 raw-deflate settings.
"""
from __future__ import annotations

import struct
import zlib

MAGIC = b"ZDX1"
VERSION = 1
CHUNK = 4096
HEADER_SIZE = 32
TABLE_ENTRY = 8
WINDOW_BITS = 12  # 4 KiB window, matches reader_zdx.dict[4096]
COMP_LEVEL = 9


def _count_chars(text: str) -> int:
    return sum(1 for c in text if not c.isspace())


def encode(raw: bytes) -> bytes:
    """Compress UTF-8 `raw` into a zdx image (raw deflate, 4 KiB chunks)."""
    chunks = [raw[i:i + CHUNK] for i in range(0, len(raw), CHUNK)] or [b""]
    comps = [zlib.compress(c, COMP_LEVEL, wbits=-WINDOW_BITS) for c in chunks]

    text = raw.decode("utf-8", errors="replace")
    chars = _count_chars(text)

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    header[4] = VERSION
    header[5] = 1  # flags: raw deflate
    struct.pack_into("<IIII", header, 8, CHUNK, len(chunks), len(raw), chars)

    table_size = len(comps) * TABLE_ENTRY
    data_start = HEADER_SIZE + table_size
    offsets = []
    pos = data_start
    for c in comps:
        offsets.append(pos)
        pos += len(c)

    out = bytearray(header)
    for off, c in zip(offsets, comps):
        out += struct.pack("<II", off, len(c))
    for c in comps:
        out += c
    return bytes(out)


def header_fields(blob: bytes) -> dict:
    if len(blob) < HEADER_SIZE or blob[:4] != MAGIC:
        raise ValueError("not a zdx image")
    chunk, count, orig, chars = struct.unpack_from("<IIII", blob, 8)
    return {
        "version": blob[4],
        "flags": blob[5],
        "chunk": chunk,
        "count": count,
        "original_size": orig,
        "text_chars": chars,
    }


def decode(blob: bytes) -> bytes:
    """Pure-python decoder used as a cross-check oracle."""
    f = header_fields(blob)
    out = bytearray()
    for i in range(f["count"]):
        off, ln = struct.unpack_from("<II", blob, HEADER_SIZE + i * TABLE_ENTRY)
        out += zlib.decompress(blob[off:off + ln], wbits=-WINDOW_BITS)
    assert len(out) == f["original_size"], (len(out), f["original_size"])
    return bytes(out)


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 3:
        print("usage: zdx_codec.py encode IN.txt OUT.zdx | decode IN.zdx OUT.txt")
        raise SystemExit(2)
    mode = sys.argv[1]
    if mode == "encode":
        data = open(sys.argv[2], "rb").read()
        open(sys.argv[3], "wb").write(encode(data))
        print(f"encoded {len(data)} -> {len(encode(data))} bytes")
    elif mode == "decode":
        data = open(sys.argv[2], "rb").read()
        open(sys.argv[3], "wb").write(decode(data))
        print(f"decoded -> {len(decode(data))} bytes")
    else:
        raise SystemExit("unknown mode")
