#!/usr/bin/env python3
"""Reference zdx encoder/decoder used by host tests and the offline preview.

The on-device decoder lives in main/reader_zdx.c; this module produces the exact
byte layout it expects and is the oracle the host tests compare against. The
transfer web page uses pako with the same windowBits=14 raw-deflate settings.

16 KiB chunks are one independent deflate stream each, so the chunk size is also
the LZ77 window: on a real book 4 KiB chunks compress to 55.3 % of the original,
16 KiB to 51.2 % (32 KiB only reaches 50.6 % for twice the dictionary buffer).
The decoder accepts any power-of-two chunk size up to READER_ZDX_CHUNK, so older
4 KiB books keep loading.
"""
from __future__ import annotations

import struct
import zlib

MAGIC = b"ZDX1"
VERSION = 1
CHUNK = 16384
HEADER_SIZE = 32
TABLE_ENTRY = 8
WINDOW_BITS = 14  # 16 KiB window, matches reader_zdx.dict[16384]
COMP_LEVEL = 9


def _count_chars(text: str) -> int:
    return sum(1 for c in text if not c.isspace())


def encode(raw: bytes, chunk: int = CHUNK, window_bits: int = WINDOW_BITS) -> bytes:
    """Compress UTF-8 `raw` into a zdx image (raw deflate, 16 KiB chunks).

    `chunk`/`window_bits` are overridable so the host tests can also produce a
    legacy 4 KiB image and prove old uploads still decode.
    """
    chunks = [raw[i:i + chunk] for i in range(0, len(raw), chunk)] or [b""]
    comps = [zlib.compress(c, COMP_LEVEL, wbits=-window_bits) for c in chunks]

    text = raw.decode("utf-8", errors="replace")
    chars = _count_chars(text)

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    header[4] = VERSION
    header[5] = 1  # flags: raw deflate
    struct.pack_into("<IIII", header, 8, chunk, len(chunks), len(raw), chars)

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
    window_bits = max(9, f["chunk"].bit_length() - 1)
    out = bytearray()
    for i in range(f["count"]):
        off, ln = struct.unpack_from("<II", blob, HEADER_SIZE + i * TABLE_ENTRY)
        out += zlib.decompress(blob[off:off + ln], wbits=-window_bits)
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
