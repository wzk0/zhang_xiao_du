#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the LVGL 9 Chinese bitmap fonts for the eBook reader.

Why an in-repo generator instead of lv_font_conv
-------------------------------------------------
The pinned LVGL version reads PLAIN bitmap fonts (bitmap_format = 0).  The
published lv_font_conv 1.5.3 produced corrupted glyph bitmaps under the Node.js
available here (bitmap_index values pointed past the end of the array), which
is also why the padding/inventory gates below exist.  Rendering
with Pillow/FreeType and emitting the LVGL structures directly removes that
toolchain from the trust path, and ``--verify`` re-parses the emitted C file and
compares every glyph, pixel by pixel, against the rasterisation that produced it.

Glyph contrast and stroke uniformity
------------------------------------
Bitmaps are packed at 1 bpp and decode to full opacity or transparency.

A single global coverage cutoff is nevertheless *phase sensitive*. FreeType's
antialiasing can give one horizontal stem a coverage of 200+ and an identical
neighbouring stem only ~100, purely because of where each falls relative to
the pixel grid. Thresholding therefore kept some strokes at 2 px, cut others
to 1 px, or broke them, and dense characters such as 銀河鐵道 / 閱讀設置
clumped into black blocks. That is the "some places have more pixels than
others" the reader reported -- a problem no threshold can fix.

All nine sizes therefore draw Hanzi from the NATIVE BITMAP STRIKES in
``assets/fonts/cjk_strikes.txt`` (12x12, 13x13, 14x14, 16x16 hand-designed
WenQuanYi bitmap-song dots). A designed dot-matrix stem is one pixel wide
everywhere, so no stroke depends on the antialiasing phase:

  * 12-15 px are FROZEN. 12/13/14 px use their native strike verbatim; 15 px
    reuses the 14x14 strike unchanged (one blank row/column). The reader
    called these sizes perfect, and tests/test_reader_font_coverage.py pins
    their emitted bitmaps by SHA-256 so a future edit cannot touch them.
  * 16 px uses the native 16x16 strike, lightly weighted (see below).
  * 17-20 px expand the 16x16 strike by inserting rows and
    columns between source pixels. An inserted pixel extends a stroke only
    where it connects to the next source row or column. Copying whole rows
    and columns would double the thickness of whichever stems land there.
  * Latin letters and Unicode punctuation use the Light face's hinted monochrome
    bitmap. The Regular face still supplies their advance widths.
  * Digits and other UI symbols keep the Regular Noto path:
    BDF punctuation boxes carry x/y offsets that would be distorted if
    stretched into the LVGL glyph box, and the hinted skeleton is crisp for
    them. That path is ``font.getmask2(ch, mode="1")`` (FreeType HINTED
    MONOCHROME bitmap) OR a grayscale stroke core >= STROKE_CORE_THRESHOLD.

Weighting on the native 16 px strike fills inner concave corners.
Other sizes skip the added corner pixels.

The strike path uses the strike's OWN square em box (or its upscaled square
box), not Noto's tight ``getbbox()``. Each ``==STRIKE`` header records the
``baseline_row``; it is scaled with the bitmap and ``ofs_y = baseline_row -
box_size`` puts every Hanzi on one baseline. Earlier code squeezed the square
strike into Noto's tight box: boxes ranged from 12x6 to 16x17, so every
character was distorted by a different factor and a single horizontal stem
was skipped entirely. Using the native box fixes both, and a square box is
SMALLER than Noto's tall tight boxes, so the shipped bitmaps shrink.

Layout is unchanged in both paths: ``adv_w`` still comes from Noto's
``getlength()``, ``line_height`` from the size, and pagination/bookmarks are
byte offsets. Only the glyph box and its pixels differ.

Usage
-----
  python3 tools/gen_reader_font.py            # generate + verify (host only)
  python3 tools/gen_reader_font.py --check    # coverage gate, no font toolchain

Character inventory
-------------------
  * printable ASCII (0x20-0x7E)
  * GB2312 level-1 Hanzi (3755), the documented supported set for uploaded books
  * Chinese punctuation, full-width space and the symbols used by the UI
  * every non-ASCII character found in a string literal under main/ and in the
    built-in books (so new UI text cannot silently ship without a glyph)

Uploaded book text is dynamic: characters outside this inventory are counted at
upload time and reported as unsupported instead of being silently dropped.  See
the font rule in AGENTS.md for the coverage contract.

The generated .c files, charset.txt and cjk_strikes.txt are committed; the
source TTF/TTC and the original PCF fonts are not. Font sources:

  * Noto Sans CJK SC (SIL Open Font License 1.1) -- Light for Latin letters and punctuation,
    Regular for digits, other UI symbols and all advance widths.
  * WenQuanYi bitmap-song, Debian package ``xfonts-wqy``
    (GPL-2-with-font-embedding-exception) -- native 12-16 px Hanzi dots,
    deterministically upscaled for 17-20 px. See
    assets/fonts/LICENSE.wqy-bitmapfont.txt. Strikes are extracted by
    tools/extract_cjk_strikes.py; only the extracted bitmap table is shipped.
"""
from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from pathlib import Path

# Pillow is imported lazily inside run_generate(): it is only needed to
# rasterise glyphs. Keeping module scope free of third-party imports lets the
# --check coverage gate run under any interpreter, including the ESP-IDF
# Python virtualenv used by tools/validate.sh, which does not ship Pillow.

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
CHARSET_FILE = FONT_DIR / "charset.txt"
DEFAULT_FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
DEFAULT_LATIN_FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Light.ttc"
DEFAULT_FACE_INDEX = 2  # Noto Sans CJK SC
BPP = 1
# Hanzi source strike for each selectable size. 12-14 px use their own native
# strike verbatim; 15 px reuses 14 px unchanged; 16 px uses 16 px native;
# 17-20 px deterministically upscale the 16 px strike to the selected size.
# 12-15 px are FROZEN: their emitted bitmaps are pinned by SHA-256 in the
# coverage test because the reader called those sizes perfect.
STRIKES_FILE = FONT_DIR / "cjk_strikes.txt"
BITMAP_STRIKE_FOR_SIZE = {12: 12, 13: 13, 14: 14, 15: 14, 16: 16,
                          17: 16, 18: 16, 19: 16, 20: 16}
# Sizes that get the inner-corner weighting (see fill_inner_corners). Below
# this the bitmap is emitted byte-for-byte as before.
HARMONIZE_MIN_SIZE = 16
# Grayscale core OR-ed onto FreeType's hinted monochrome skeleton for ASCII,
# punctuation and UI symbols. 200 keeps only the centre of genuinely thick
# strokes; lowering it towards 88 would reintroduce the phase-sensitive edge
# pixels. Pinned by tests/test_reader_font_coverage.py.
STROKE_CORE_THRESHOLD = 200
# 12 px is the smallest size at which 2-bit CJK glyphs stay readable; 20 px is
# the largest that still fits a comfortable page on the 240x320 panel.
# Selectable body sizes. 17 px and 19 px are dropped: both are deterministic
# upscales of the 16 px strike that sat between the cleaner 18 px and 20 px, and
# each cost ~180-215 KiB of Flash the book store needs more. reader_layout.h
# carries the same list for the firmware.
SIZES = (12, 13, 14, 15, 16, 18, 20)
EXTRA_LEADING = 4  # line_height = size + EXTRA_LEADING, a layout parameter

# Chinese punctuation and UI symbols. U+3000 (ideographic space) must be a real
# blank glyph with a full-width advance; excluding it produces rows of tofu.
COMMON_SYMBOLS = (
    "，。！？：；、‘’“”（）【】《》〈〉「」『』…—·～￥％＋－×÷"
    "　"  # U+3000 ideographic space
    "↑↓←→✓✗●○★☆■□▲▼§¶℃°±"
    "①②③④⑤⑥⑦⑧⑨⑩第章节回页共剩余空已"
    "0123456789"  # explicit, harmless duplicate of ASCII
)

SOURCE_GLOBS = ("main/*.c", "main/*.h")
# Built-in book text now lives as canonical UTF-8 files (compressed to zdx at
# build time). They must stay in the font inventory or rare characters would
# turn into tofu after the C literals are removed.
BOOK_TEXT_GLOB = "assets/books/*.txt"
STRING_LITERAL_RE = re.compile(r'"((?:\\.|[^"\\])*)"')


def gb2312_level1() -> list[str]:
    """Enumerate the 3755 level-1 GB2312 Hanzi, the supported CJK set."""
    chars: list[str] = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and 0x4E00 <= ord(ch) <= 0x9FA5:
                chars.append(ch)
    return chars


def source_literal_chars() -> set[str]:
    """Collect non-ASCII characters from C string literals and book text."""
    found: set[str] = set()
    for pattern in SOURCE_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            text = path.read_text(encoding="utf-8")
            for literal in STRING_LITERAL_RE.findall(text):
                # Ignore escaped C sequences such as \n; keep real UTF-8 bytes.
                cleaned = re.sub(r"\\(?:[abfnrtv\\\"'?]|[0-7]{1,3}|x[0-9A-Fa-f]+|u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8})", "", literal)
                for ch in cleaned:
                    if ord(ch) > 0x7F:
                        found.add(ch)
    for path in sorted(ROOT.glob(BOOK_TEXT_GLOB)):
        for ch in path.read_text(encoding="utf-8"):
            if ord(ch) > 0x7F:
                found.add(ch)
    return found


def build_charset() -> tuple[list[str], list[str]]:
    """Return (ascii_chars, cjk_chars), both sorted and deduplicated."""
    ascii_chars = [chr(c) for c in range(0x20, 0x7F)]
    cjk = set(gb2312_level1())
    cjk.update(ch for ch in COMMON_SYMBOLS if ord(ch) > 0x7F)
    cjk.update(source_literal_chars())
    # LVGL needs a real blank glyph for the ideographic space.
    cjk.discard("\n")
    cjk.discard("\r")
    cjk.discard("\t")
    cjk_chars = sorted(cjk, key=ord)
    return ascii_chars, cjk_chars


def split_sparse_cmap_chars(chars: list[str]) -> list[list[str]]:
    """Split sorted code points into SPARSE_TINY groups (uint16 offsets)."""
    groups: list[list[str]] = []
    for ch in chars:
        if not groups or ord(ch) - ord(groups[-1][0]) > 0xFFFF:
            groups.append([])
        groups[-1].append(ch)
    return groups


def pack_bits(vals: list[int], bpp: int) -> bytes:
    """Pack pixels MSB-first into bytes; each glyph starts on a byte boundary."""
    out = bytearray()
    acc = nbits = 0
    for v in vals:
        acc = (acc << bpp) | (v & ((1 << bpp) - 1))
        nbits += bpp
        while nbits >= 8:
            nbits -= 8
            out.append((acc >> nbits) & 0xFF)
    if nbits:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def load_strikes() -> tuple[dict[int, dict[int, list[list[int]]]], dict[int, int]]:
    """Parse the committed native dot-matrix strikes (standard library only).

    Sections are ``==STRIKE size=<px> ... baseline_row=<row>`` followed by
    ``U+XXXX <w> <h> <hex bits>`` lines holding a row-major MSB-first bitmap.
    Returns (strikes, baseline_row_by_size). ``baseline_row`` is the row of
    the strike's square em box that sits on the font baseline; it is emitted
    by tools/extract_cjk_strikes.py from the BDF's BBX offsets.
    """
    if not STRIKES_FILE.is_file():
        raise RuntimeError(f"missing native bitmap strikes: {STRIKES_FILE}")
    strikes: dict[int, dict[int, list[list[int]]]] = {}
    baselines: dict[int, int] = {}
    current: int | None = None
    for line in STRIKES_FILE.read_text(encoding="utf-8").splitlines():
        if line.startswith("==STRIKE"):
            m = re.search(r"size=(\d+)", line)
            b = re.search(r"baseline_row=(-?\d+)", line)
            if not m or not b:
                raise RuntimeError(f"bad strike header (need size + baseline_row): {line}")
            current = int(m.group(1))
            strikes[current] = {}
            baselines[current] = int(b.group(1))
            continue
        if not line or line.startswith("#"):
            continue
        m = re.match(r"U\+([0-9A-Fa-f]{4,6})\s+(\d+)\s+(\d+)\s+([0-9A-Fa-f]+)\s*$", line)
        if not m:
            raise RuntimeError(f"bad strike line: {line}")
        if current is None:
            raise RuntimeError("strike data before any ==STRIKE header")
        cp, w, h, hexbits = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), m.group(4)
        if len(hexbits) * 4 < w * h:
            raise RuntimeError(
                f"U+{cp:04X}: {len(hexbits)*4} bits of hex for a {w}x{h} glyph; "
                "the strike table lost leading zeros")
        bits = bin(int(hexbits, 16))[2:].zfill(len(hexbits) * 4)[: w * h]
        strikes[current][cp] = [
            [1 if bits[r * w + c] == "1" else 0 for c in range(w)]
            for r in range(h)
        ]
    for size in sorted(set(BITMAP_STRIKE_FOR_SIZE.values())):
        if size not in strikes or not strikes[size]:
            raise RuntimeError(f"missing or empty {size}px native strike")
        if size not in baselines:
            raise RuntimeError(f"missing baseline_row for {size}px native strike")
    return strikes, baselines


def stretch_strike_rows(rows, src: int, dst: int) -> list[list[int]]:
    """Spread source rows across the target and bridge connected strokes."""
    positions = [(r * (dst - 1) + (src - 1) // 2) // (src - 1)
                 for r in range(src)]
    out = [list(rows[0])]
    for r in range(1, src):
        upper, lower = rows[r - 1], rows[r]
        if positions[r] - positions[r - 1] == 2:
            # An existing straight path already joins neighbouring pixels
            # across the gap. Add a diagonal bridge only when both possible
            # straight paths are absent.
            bridge = [a & b for a, b in zip(upper, lower)]
            for c, pixel in enumerate(upper):
                if not pixel or bridge[c]:
                    continue
                if any(lower[n] and not upper[n]
                       for n in (c - 1, c + 1) if 0 <= n < len(upper)):
                    bridge[c] = 1
            out.append(bridge)
        out.append(list(lower))
    return out


def upscale_strike(rows, src: int, dst: int) -> list[list[int]]:
    expanded = stretch_strike_rows(rows, src, dst)
    columns = stretch_strike_rows(list(zip(*expanded)), src, dst)
    return [list(row) for row in zip(*columns)]


def fill_inner_corners(rows, size: int) -> list[list[int]]:
    """Weight >= 16 px Hanzi without ever thickening a straight stem.

    A bitmap-Song dot face has 1 px horizontal stems and 2 px vertical ones
    (measured 2-px-run ratio 1.89 at 16 px), which reads as pale and uneven.
    Fill ONLY the inner concave corner: an empty pixel whose left and upper
    neighbours are ink while the upper-left diagonal is empty. Reads come from
    the unmodified bitmap, so the pass is deterministic and single-pass; no
    straight stem becomes 2 px and 2x2 black blocks stay near zero (0.2 ->
    1.2 per glyph at 16 px). Phase-free by construction, unlike the grayscale
    core it replaces. 12-15 px skip this entirely.
    """
    out = [r[:] for r in rows]
    for r in range(1, size):
        for c in range(1, size):
            if (not rows[r][c] and rows[r][c - 1] and rows[r - 1][c]
                    and not rows[r - 1][c - 1]):
                out[r][c] = 1
    return out


def strike_glyph(ch: str, info: dict) -> tuple[int, int, int, int, list[int]] | None:
    """Full glyph geometry and binary coverage for a Hanzi in its OWN square box.

    The square strike box (or its deterministic upscale) is used verbatim: no
    cropping into Noto's tight bbox. The baseline row scales with the bitmap
    and ``ofs_y = baseline_row - box_size`` puts every Hanzi on one baseline,
    so sparse glyphs keep their single horizontal stem at its true height and
    Hanzi stay aligned with the Noto ASCII/punctuation glyphs. ``adv_w`` is
    still supplied by Noto, so pagination and bookmarks never move.
    """
    cp = ord(ch)
    if not (0x4E00 <= cp <= 0x9FFF):
        return None
    rows = info["rows"].get(cp)
    if rows is None:
        return None
    src = info["size"]
    dst = info["target_size"]
    if len(rows) != src or any(len(r) != src for r in rows):
        raise RuntimeError(f"U+{cp:04X}: strike box is not {src}x{src}")
    if info["harmonize"]:
        rows = fill_inner_corners(rows, src)
    if dst != src:
        rows = upscale_strike(rows, src, dst)
    baseline_row = (info["baseline_row"] * dst + src // 2) // src
    vals = [v for row in rows for v in row]
    return dst, dst, 0, baseline_row - dst, vals


def render_glyph(font, latin_font, ch: str, ascent: int, bpp: int, strike_info=None):
    """Return (bitmap, box_w, box_h, ofs_x, ofs_y, adv_w, pixel_values).

    ``strike_info`` is ``{"size", "rows", "baseline_row"}`` for this size, or
    None for the Noto path. Advance width still comes from Noto in BOTH cases,
    so switching the rasterisation source never changes pagination.
    """
    adv_w = int(round(font.getlength(ch) * 16))  # LVGL adv_w unit = 1/16 px

    # Small Hanzi: native square dot-matrix box, placed on the shared baseline.
    # This must run BEFORE Noto's getbbox(): Noto's tight box is deliberately
    # not used here (see strike_glyph's docstring).
    if strike_info is not None:
        g = strike_glyph(ch, strike_info)
        if g is not None:
            w, h, ox, oy, vals = g
            return pack_bits(vals, bpp), w, h, ox, oy, adv_w, vals

    latin = ("A" <= ch <= "Z") or ("a" <= ch <= "z")
    light = latin or unicodedata.category(ch).startswith("P")
    if light:
        font = latin_font
    bbox = font.getbbox(ch)
    if bbox is None:
        return b"", 0, 0, 0, 0, adv_w, []
    x0, y0, x1, y1 = bbox
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:  # space and other ink-free glyphs
        return b"", 0, 0, 0, 0, adv_w, []
    # LVGL 9.5: glyph bottom = baseline - ofs_y, so ofs_y = ascent - bbox.bottom.
    ofs_y = ascent - y1

    # FreeType's hinted monochrome bitmap snaps every stem onto the integer
    # pixel grid. Pillow returns an ImagingCore of 0/255 bytes in row-major
    # order; its size equals getbbox() for every code point in the inventory,
    # so no geometry (box, advance, baseline) changes -- only the pixels do.
    mono_mask, _mono_off = font.getmask2(ch, mode="1")
    mono = list(mono_mask)
    if len(mono) != w * h:
        raise RuntimeError(f"hinted bitmap size mismatch for U+{ord(ch):04X}")
    if light:
        vals = [int(bool(pixel)) for pixel in mono]
    else:
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255)
        vals = [int(bool(m) or p >= STROKE_CORE_THRESHOLD) for p, m in zip(img.tobytes(), mono)]
    return pack_bits(vals, bpp), w, h, x0, ofs_y, adv_w, vals


def emit_font(size: int, out_path: Path, font, latin_font, ascii_chars, cjk_chars, strike_info=None):
    ascent, descent = font.getmetrics()
    line_height = size + EXTRA_LEADING
    all_chars = ascii_chars + cjk_chars

    bitmap = bytearray()
    dscs = [(0, 0, 0, 0, 0, 0)]  # glyph id 0 is reserved
    expected_pixels: list[list[int]] = [[]]
    for ch in all_chars:
        bm, bw, bh, ox, oy, aw, vals = render_glyph(font, latin_font, ch, ascent, BPP, strike_info)
        dscs.append((len(bitmap), aw, bw, bh, ox, oy))
        bitmap += bm
        expected_pixels.append(vals)

    groups = split_sparse_cmap_chars(cjk_chars)
    ascii_gid_start = 1
    cjk_gid_start = 1 + len(ascii_chars)

    L: list[str] = []
    L.append("// Generated by tools/gen_reader_font.py; do not edit manually.")
    L.append("// Sources: Hanzi = WenQuanYi bitmap-song native strikes")
    L.append("//          (GPL-2-with-font-embedding-exception, shipped as cjk_strikes.txt);")
    L.append("//          Latin letters and punctuation = Noto Sans CJK SC Light (OFL 1.1).")
    L.append("//          Digits and other symbols = Noto Sans CJK SC Regular (OFL 1.1).")
    L.append(f"// size={size}px bpp={BPP} line_height={line_height} base_line={descent}")
    if strike_info is not None:
        src, dst = strike_info["size"], strike_info["target_size"]
        raster = f"native {src}px dot-matrix strike"
        if dst != src:
            raster += f" expanded to {dst}px with stroke bridges"
        if strike_info["harmonize"]:
            raster += " + inner-corner weighting"
        L.append(f"// Hanzi raster: {raster}")
    else:
        L.append("// Hanzi raster: Noto hinted monochrome skeleton")
    L.append(f"// ASCII(0x20-0x7E) + {len(cjk_chars)} CJK/symbol code points (GB2312 level 1).")
    L.append("// PLAIN bitmap format (bitmap_format=0), const in .rodata / flash.")
    L.append('#include "lvgl.h"')
    L.append("")
    L.append("static const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(bitmap), 24):
        L.append("    " + "".join(f"0x{b:02X}," for b in bitmap[i:i + 24]))
    L.append("};")
    L.append("")
    L.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    for (bi, aw, bw, bh, ox, oy) in dscs:
        L.append(f"    {{.bitmap_index = {bi}, .adv_w = {aw}, .box_w = {bw}, "
                 f".box_h = {bh}, .ofs_x = {ox}, .ofs_y = {oy}}},")
    L.append("};")
    L.append("")

    for gi, group in enumerate(groups):
        if len(group) == 1:
            continue
        start = ord(group[0])
        offs = [ord(ch) - start for ch in group]
        L.append(f"static const uint16_t unicode_list_cjk_{gi}[] = {{")
        for i in range(0, len(offs), 12):
            L.append("    " + "".join(f"0x{o:04X}, " for o in offs[i:i + 12]).rstrip())
        L.append("};")
        L.append("")

    L.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    L.append("    {")
    L.append(f"        .range_start = 32, .range_length = {len(ascii_chars)}, "
             f".glyph_id_start = {ascii_gid_start},")
    L.append("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,")
    L.append("        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
    L.append("    },")
    gid = cjk_gid_start
    for gi, group in enumerate(groups):
        start, end = ord(group[0]), ord(group[-1])
        L.append("    {")
        L.append(f"        .range_start = {start}, .range_length = {end - start + 1}, "
                 f".glyph_id_start = {gid},")
        if len(group) == 1:
            L.append("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,")
            L.append("        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY")
        else:
            L.append(f"        .unicode_list = unicode_list_cjk_{gi}, .glyph_id_ofs_list = NULL, "
                     f".list_length = {len(group)},")
            L.append("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY")
        gid += len(group)
        L.append("    },")
    L.append("};")
    L.append("")
    L.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    L.append("    .glyph_bitmap = glyph_bitmap,")
    L.append("    .glyph_dsc = glyph_dsc,")
    L.append("    .cmaps = cmaps,")
    L.append("    .kern_dsc = NULL,")
    L.append("    .kern_scale = 0,")
    L.append(f"    .cmap_num = {1 + len(groups)},")
    L.append(f"    .bpp = {BPP},")
    L.append("    .kern_classes = 0,")
    L.append("    .bitmap_format = 0,")
    L.append("};")
    L.append("")
    L.append(f"const lv_font_t lv_font_reader_{size} = {{")
    L.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    L.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    L.append(f"    .line_height = {line_height},")
    L.append(f"    .base_line = {descent},")
    L.append("    .subpx = LV_FONT_SUBPX_NONE,")
    L.append("    .underline_position = -2,")
    L.append("    .underline_thickness = 1,")
    L.append("    .dsc = &font_dsc,")
    L.append("    .fallback = NULL,")
    L.append("    .user_data = NULL,")
    L.append("};")
    L.append("")

    out_path.write_text("\n".join(L), encoding="utf-8")
    return dscs, bytes(bitmap), expected_pixels, groups


def verify_font(path: Path, size: int, ascii_chars, cjk_chars, dscs, bitmap, expected_pixels, groups):
    """Re-parse the emitted C file and compare every glyph against FreeType."""
    text = path.read_text(encoding="utf-8")

    m = re.search(r"glyph_bitmap\[\] = \{(.*?)\};", text, re.S)
    if not m:
        raise RuntimeError(f"{path.name}: cannot find glyph_bitmap")
    parsed_bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    if parsed_bitmap != bitmap:
        raise RuntimeError(f"{path.name}: bitmap array changed during emission")

    d = re.search(r"glyph_dsc\[\] = \{(.*?)\};", text, re.S)
    if not d:
        raise RuntimeError(f"{path.name}: cannot find glyph_dsc")
    parsed = [tuple(map(int, r)) for r in re.findall(
        r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
        r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", d.group(1))]
    if parsed != dscs:
        raise RuntimeError(f"{path.name}: glyph descriptors changed during emission")

    all_chars = ascii_chars + cjk_chars
    if len(parsed) != len(all_chars) + 1:
        raise RuntimeError(f"{path.name}: glyph count mismatch")

    for gid, (bi, aw, bw, bh, ox, oy) in enumerate(parsed[1:], start=1):
        vals = expected_pixels[gid]
        if bw == 0 or bh == 0:
            if vals:
                raise RuntimeError(f"{path.name}: ink-free glyph {all_chars[gid-1]!r} has pixels")
            continue
        nbytes = (bw * bh * BPP + 7) // 8
        raw = parsed_bitmap[bi:bi + nbytes]
        if len(raw) != nbytes:
            raise RuntimeError(f"{path.name}: truncated bitmap for glyph {gid}")
        decoded = [(byte >> shift) & 1 for byte in raw for shift in range(7, -1, -1)]
        if decoded[: len(vals)] != list(vals):
            raise RuntimeError(f"{path.name}: pixel mismatch for {all_chars[gid-1]!r} (glyph {gid})")

    # cmap coverage: every requested code point must be reachable. ASCII is
    # covered by the FORMAT0_TINY cmap (32..126); the rest by SPARSE_TINY lists.
    emitted = {ord(c) for c in ascii_chars}
    for gi, group in enumerate(groups):
        if len(group) == 1:
            body = ""
        else:
            um = re.search(rf"unicode_list_cjk_{gi}\[\] = \{{(.*?)\}};", text, re.S)
            if not um:
                raise RuntimeError(f"{path.name}: missing unicode_list_cjk_{gi}")
            body = um.group(1)
        offs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", body)] if len(group) > 1 else [0]
        start = ord(group[0])
        emitted.update(start + o for o in offs)
    expected = {ord(c) for c in cjk_chars} | {ord(c) for c in ascii_chars}
    if emitted != expected:
        missing = sorted(expected - emitted)[:8]
        raise RuntimeError(f"{path.name}: cmap misses code points {missing}")

    print(f"  verify {path.name}: {len(parsed)-1} glyphs, "
          f"{len(bitmap)/1024:.1f} KiB bitmap, line_height={size + EXTRA_LEADING} OK")


CHARSET_HEADER = FONT_DIR / "reader_charset.h"


def write_charset(ascii_chars, cjk_chars):
    codepoints = sorted({ord(c) for c in ascii_chars + cjk_chars})
    lines = [
        "# Committed character inventory for assets/fonts/lv_font_reader_*.c",
        "# One code point per line: U+XXXX. Generated by tools/gen_reader_font.py.",
        "# The coverage test (tests/test_reader_font_coverage.py) reads this file;",
        "# it needs no font toolchain at validation time.",
    ]
    lines += [f"U+{cp:04X}" for cp in codepoints]
    CHARSET_FILE.write_text("\n".join(lines) + "\n", encoding="utf-8")

    # A plain C table lets the network/upload task check coverage without ever
    # touching LVGL (which is not thread-safe and must be accessed under its lock).
    h: list[str] = [
        "// Generated by tools/gen_reader_font.py; do not edit manually.",
        "// Committed code-point inventory, mirrored from charset.txt.",
        "#pragma once",
        "#include <stdint.h>",
        f"#define READER_CHARSET_COUNT {len(codepoints)}",
        "static const uint32_t reader_charset_codepoints[READER_CHARSET_COUNT] = {",
    ]
    for i in range(0, len(codepoints), 12):
        h.append("    " + "".join(f"0x{cp:04X}, " for cp in codepoints[i:i + 12]).rstrip())
    h += [
        "};",
        "static inline int reader_charset_has(uint32_t cp) {",
        "    uint32_t lo = 0, hi = READER_CHARSET_COUNT;",
        "    while (lo < hi) {",
        "        uint32_t mid = lo + (hi - lo) / 2;",
        "        uint32_t v = reader_charset_codepoints[mid];",
        "        if (v == cp) return 1;",
        "        if (v < cp) lo = mid + 1; else hi = mid;",
        "    }",
        "    return 0;",
        "}",
        "",
    ]
    CHARSET_HEADER.write_text("\n".join(h), encoding="utf-8")
    return len(codepoints)


def run_generate(font_path: str, latin_font_path: str, face_index: int):
    # Lazy import: the --check gate above never reaches this point, so a host
    # without Pillow can still validate the committed font inventory.
    global Image, ImageDraw, ImageFont
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("Pillow is required to regenerate fonts: pip3 install pillow",
              file=sys.stderr)
        raise SystemExit(1)

    for source in (font_path, latin_font_path):
        if not Path(source).is_file():
            sys.exit(f"Font not found: {source}")
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    strikes, baselines = load_strikes()
    ascii_chars, cjk_chars = build_charset()
    print(f"Character inventory: {len(ascii_chars)} ASCII + {len(cjk_chars)} CJK/symbols")
    print(f"Vector fonts: {font_path}, {latin_font_path} (face index {face_index})")
    print(f"Native strikes: {', '.join(f'{s}px={len(strikes[s])} baseline_row={baselines[s]}' for s in sorted(strikes))}")
    advances = []
    for size in SIZES:
        font = ImageFont.truetype(font_path, size, index=face_index)
        latin_font = ImageFont.truetype(latin_font_path, size, index=face_index)
        strike_info = None
        if size in BITMAP_STRIKE_FOR_SIZE:
            ss = BITMAP_STRIKE_FOR_SIZE[size]
            # 12-15 px are frozen: native strike box verbatim, no weighting.
            # 16 px uses its native 16x16 box; 17-20 px upscale it to size.
            target = ss if size < HARMONIZE_MIN_SIZE else size
            strike_info = {"size": ss, "target_size": target,
                           "rows": strikes[ss], "baseline_row": baselines[ss],
                           "harmonize": size == HARMONIZE_MIN_SIZE}
        out = FONT_DIR / f"lv_font_reader_{size}.c"
        dscs, bitmap, pixels, groups = emit_font(size, out, font, latin_font, ascii_chars, cjk_chars, strike_info)
        verify_font(out, size, ascii_chars, cjk_chars, dscs, bitmap, pixels, groups)
        advances.append([glyph[1] for glyph in dscs[1:len(ascii_chars) + 1]])
    advance_header = FONT_DIR / "reader_ascii_advance.h"
    rows = ["// Generated by tools/gen_reader_font.py. Units are 1/16 pixel.",
            "#pragma once", "#include <stdint.h>",
            "static const uint16_t reader_ascii_advance[][95] = {"]
    for row in advances:
        rows.append("    {")
        for i in range(0, len(row), 15):
            rows.append("        " + ", ".join(map(str, row[i:i + 15])) + ",")
        rows.append("    },")
    advance_header.write_text("\n".join(rows + ["};", ""]), encoding="utf-8")
    n = write_charset(ascii_chars, cjk_chars)
    print(f"Wrote {CHARSET_FILE.relative_to(ROOT)} ({n} code points)")


def run_check() -> int:
    """Coverage gate: every non-ASCII literal character must be in the inventory."""
    if not CHARSET_FILE.exists():
        print(f"ERROR: missing {CHARSET_FILE}", file=sys.stderr)
        return 1
    inventory = {
        int(m.group(1), 16)
        for m in re.finditer(r"(?m)^U\+([0-9A-Fa-f]{4,6})\s*$", CHARSET_FILE.read_text(encoding="utf-8"))
    }
    if not inventory:
        print("ERROR: empty character inventory", file=sys.stderr)
        return 1
    missing = sorted({ord(c) for c in source_literal_chars()} - inventory)
    if missing:
        print("ERROR: characters used in main/ string literals are missing from the font:", file=sys.stderr)
        print("  " + " ".join(f"U+{cp:04X} {chr(cp)}" for cp in missing[:40]), file=sys.stderr)
        print("  Regenerate with tools/gen_reader_font.py", file=sys.stderr)
        return 1
    print(f"Coverage check: PASS ({len(inventory)} code points in inventory)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--font", default=DEFAULT_FONT, help="Regular TTF/OTF/TTC source font")
    ap.add_argument("--latin-font", default=DEFAULT_LATIN_FONT,
                    help="Light TTF/OTF/TTC source font for Latin letters")
    ap.add_argument("--face-index", type=int, default=DEFAULT_FACE_INDEX,
                    help="face index inside a TTC (2 = Noto Sans CJK SC)")
    ap.add_argument("--check", action="store_true",
                    help="coverage gate only; does not need Pillow or the source font")
    args = ap.parse_args()
    if args.check:
        return run_check()
    run_generate(args.font, args.latin_font, args.face_index)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
