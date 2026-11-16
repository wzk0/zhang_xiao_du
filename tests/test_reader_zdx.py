#!/usr/bin/env python3
"""Generate deterministic zdx fixtures, compile the on-device decoder tests,
and run them. This is the host-side counterpart of reader_zdx.c."""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import zdx_codec as z  # noqa: E402


def make_fixtures(d: Path) -> list[str]:
    unit = (
        "银河铁道之夜是宫泽贤治的童话，讲述少年在银河列车上的奇妙旅程。\n"
        "窗外星光闪烁，车厢里坐着形形色色的乘客，每个人都有自己的故事。\n"
        "Chapter 42: the night train 123 goes on.\n"
    )
    raw = (unit * 120).encode("utf-8")
    (d / "good.txt").write_bytes(raw)
    blob = z.encode(raw)
    (d / "good.zdx").write_bytes(blob)

    def variant(name, mutate):
        b = bytearray(blob)
        mutate(b)
        (d / name).write_bytes(b)
        return str(d / name)

    bad_magic = variant("bad_magic.zdx", lambda b: b.__setitem__(0, ord("X")))
    bad_version = variant("bad_version.zdx", lambda b: b.__setitem__(4, 99))
    bad_size = variant(
        "bad_size.zdx",
        lambda b: struct.pack_into("<I", b, 16, 0x7FFFFFFF),
    )
    (d / "truncated.zdx").write_bytes(blob[: len(blob) // 2])
    bad_table = variant(
        "bad_table.zdx",
        lambda b: struct.pack_into("<I", b, z.HEADER_SIZE + 4, 0x7FFFFFFF),
    )

    # The python decoder must agree with the fixture too (encoder oracle).
    assert z.decode(blob) == raw
    return [str(d / "good.zdx"), str(d / "good.txt"),
            bad_magic, bad_version, bad_size, str(d / "truncated.zdx"), bad_table]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="zdx-fixtures-") as td:
        args = make_fixtures(Path(td))
        test_dir = Path(td) / "bin"
        test_dir.mkdir()
        cc = os.environ.get("CC", "cc")
        binary = str(test_dir / "test_reader_zdx")
        cmd = [
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I" + str(ROOT / "main"),
            "-I" + str(ROOT / "components" / "uzlib"),
            str(ROOT / "tests" / "test_reader_zdx.c"),
            str(ROOT / "main" / "reader_zdx.c"),
            str(ROOT / "components" / "uzlib" / "tinflate.c"),
            str(ROOT / "components" / "uzlib" / "crc32.c"),
            str(ROOT / "components" / "uzlib" / "adler32.c"),
            "-o", binary,
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([binary] + args, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
