#!/usr/bin/env python3
"""Font coverage gate for the eBook reader.

The generated bitmap fonts can only draw the code points committed in
assets/fonts/charset.txt. This test needs no font toolchain (no Pillow, no TTF)
and fails when:

  * a C string literal under main/ uses a character the font lacks;
  * a generated font .c file no longer matches the inventory;
  * a font regresses to ANY grey level (antialiasing, "ink boost" fringes or
    fattened strokes all read as blurry Hanzi on this panel);
  * OR too many stroke pixels are deleted (characters turn skeletal);
  * OR the generator drops FreeType's hinted monochrome skeleton: stem widths
    then depend on the antialiasing phase again, so identical strokes come out
    2 px wide, 1 px wide, or broken ("some places have more pixels");
  * OR the native dot-matrix strikes for the small sizes are missing: Hanzi at
    every size must be drawn from designed 1-pixel stems, not from a scaled
    vector bitmap (which is what made small text clump into black blocks);
  * OR the square strike is squeezed into Noto's tight glyph box again: that
    distorts each character by a different factor and deletes the single
    horizontal stem of 一 entirely;
  * OR glyphs other than punctuation change: their pixels and advances are pinned;
  * OR 16-20 px regresses to phase-sensitive stems: the old grayscale core
    produced 2x2 black blocks and horizontal/vertical stems of different
    widths ("some places thicker, some thinner"), so a sampled stroke-balance
    and black-block ceiling is enforced;
  * the negative control is accidentally added to the inventory (a check that
    cannot fail is worse than no check).

Run standalone or from tools/validate.sh --static.
"""
from __future__ import annotations

import hashlib
import re
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
CHARSET_FILE = FONT_DIR / "charset.txt"
LAYOUT_HEADER = ROOT / "main" / "reader_layout.h"
LAYOUT_SOURCE = ROOT / "main" / "reader_layout.c"
CHARSET_HEADER = ROOT / "assets" / "fonts" / "reader_charset.h"
STRING_LITERAL_RE = re.compile(r'"((?:\\.|[^"\\])*)"')
ESCAPE_RE = re.compile(
    r"\\(?:[abfnrtv\\\"'?]|[0-7]{1,3}|x[0-9A-Fa-f]+|u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8})"
)
INVENTORY_RE = re.compile(r"(?m)^U\+([0-9A-Fa-f]{4,6})\s*$")
NEGATIVE_CONTROL = 0x1F600  # emoji: deliberately outside the reader's inventory


def font_sizes() -> tuple[int, ...]:
    """Selectable body sizes, read from the table the firmware compiles.

    Deriving the list here keeps this gate honest when a size is added or
    removed: a missing row shows up as a missing font file, not as a silently
    untested size. The sizes are no longer a contiguous range (17 px and 19 px
    were dropped as upscaled duplicates), so the emitted table is the source of
    truth and READER_FONT_SIZE_COUNT must agree with it.
    """
    text = LAYOUT_SOURCE.read_text(encoding="utf-8")
    table = re.search(r"g_reader_font_sizes\[[^\]]*\]\s*=\s*\{([^}]*)\}", text)
    if not table:
        raise RuntimeError(f"{LAYOUT_SOURCE.name}: cannot find g_reader_font_sizes")
    sizes = tuple(int(value) for value in re.findall(r"\d+", table.group(1)))
    header = LAYOUT_HEADER.read_text(encoding="utf-8")
    count = re.search(r"#define READER_FONT_SIZE_COUNT\s+(\d+)", header)
    if not count:
        raise RuntimeError(f"{LAYOUT_HEADER.name}: cannot find READER_FONT_SIZE_COUNT")
    if len(sizes) != int(count.group(1)):
        raise RuntimeError(
            f"{LAYOUT_SOURCE.name}: {len(sizes)} sizes but READER_FONT_SIZE_COUNT "
            f"is {count.group(1)}")
    if not sizes or list(sizes) != sorted(set(sizes)):
        raise RuntimeError(f"{LAYOUT_SOURCE.name}: sizes must be unique and ascending")
    return sizes


def font_files(sizes: tuple[int, ...]) -> tuple[Path, ...]:
    return tuple(FONT_DIR / f"lv_font_reader_{size}.c" for size in sizes)


def stale_font_files(sizes: tuple[int, ...]) -> list[str]:
    """Generated fonts that no selectable size refers to (leftover artefacts)."""
    expected = {p.name for p in font_files(sizes)}
    return sorted(p.name for p in FONT_DIR.glob("lv_font_reader_*.c")
                  if p.name not in expected)


def read_inventory() -> set[int]:
    if not CHARSET_FILE.is_file():
        raise RuntimeError(f"missing character inventory: {CHARSET_FILE}")
    return {int(m.group(1), 16)
            for m in INVENTORY_RE.finditer(CHARSET_FILE.read_text(encoding="utf-8"))}


def source_chars() -> set[str]:
    found: set[str] = set()
    for path in sorted((ROOT / "main").glob("*.[ch]")):
        text = path.read_text(encoding="utf-8")
        for literal in STRING_LITERAL_RE.findall(text):
            cleaned = ESCAPE_RE.sub("", literal)
            for ch in cleaned:
                if ord(ch) > 0x7F:
                    found.add(ch)
    return found


def missing(chars: set[str], inventory: set[int]) -> list[int]:
    return sorted({ord(c) for c in chars} - inventory)


def check_font_file(path: Path, inventory: set[int]) -> None:
    if not path.is_file():
        raise RuntimeError(f"missing generated font: {path}")
    text = path.read_text(encoding="utf-8")

    dsc = re.search(r"glyph_dsc\[\] = \{(.*?)\};", text, re.S)
    if not dsc:
        raise RuntimeError(f"{path.name}: cannot find glyph_dsc")
    glyph_count = len(re.findall(r"\{\.bitmap_index", dsc.group(1)))
    if glyph_count != len(inventory) + 1:  # +1 for the reserved glyph 0
        raise RuntimeError(
            f"{path.name}: {glyph_count - 1} glyphs but inventory has "
            f"{len(inventory)} code points; regenerate with tools/gen_reader_font.py"
        )

    # Rebuild the cmap coverage: ASCII FORMAT0_TINY (32..126) plus every
    # unicode_list offset relative to its group's range_start.
    emitted: set[int] = set(range(0x20, 0x7F))
    cmaps = re.search(r"cmaps\[\] = \{(.*?)\};", text, re.S)
    if not cmaps:
        raise RuntimeError(f"{path.name}: cannot find cmaps")
    starts = [int(m.group(1), 0) for m in
              re.finditer(r"\.range_start = (\d+)", cmaps.group(1))]
    group_idx = 0
    for name, body in re.findall(r"unicode_list_(\w+)\[\] = \{(.*?)\};", text, re.S):
        start = starts[1 + group_idx] if 1 + group_idx < len(starts) else 0
        offs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", body)]
        emitted.update(start + o for o in offs)
        group_idx += 1
    if emitted != inventory:
        absent = sorted(inventory - emitted)[:8]
        raise RuntimeError(
            f"{path.name}: cmap does not cover the inventory, e.g. "
            + " ".join(f"U+{cp:04X}" for cp in absent)
        )


GLYPH_BITMAP_RE = re.compile(r"glyph_bitmap\[\] = \{(.*?)\};", re.S)
GENERATOR = ROOT / "tools" / "gen_reader_font.py"
STROKE_CORE_THRESHOLD_RE = re.compile(
    r"^STROKE_CORE_THRESHOLD\s*=\s*(\d+)\s*$", re.M)
# FreeType's hinted monochrome bitmap is what makes every stem the same width.
HINTED_MONO_RE = re.compile(r'getmask2\(ch, mode="1"\)')

# Crisp-font contract. LVGL's opa2_table is {0, 85, 170, 255}: levels 1/2 are
# the grey haze that makes 12-20 px CJK stems look blurry, and the old "ink
# boost" (coverage >= 64 -> solid) fattened them into blobs. The generator
# therefore emits ONLY levels 0 and 3, a pure bilevel bitmap.
#
# ALL nine sizes draw Hanzi from the designed native dot-matrix strikes:
# 12-14 px native, 15 px = 14 px strike, 16 px native, and 17-20 px as a
# stroke-preserving expansion of the 16 px strike. Only ASCII,
# punctuation and UI symbols still use FreeType's hinted monochrome skeleton
# plus a grayscale stroke core. Pinned here:
#   (1) a pure bitmap can be too sparse -- per-size ink-pixel floors are kept;
#   (2) the hinted skeleton must stay in the generator for the glyphs that
#       still use it, and the core cutoff must stay in the stroke-core band;
#   (3) the native strikes must cover every Hanzi in the inventory;
#   (4) approved glyphs and advances are frozen by SHA-256;
#   (5) 16-20 px must keep balanced horizontal/vertical stems and a low 2x2
#       black-block count -- the old grayscale core made them phase-sensitive.
CORE_MIN, CORE_MAX = 160, 224
MIN_INK_RATIO = 0.90
# Solid-ink counts for the native and expanded strikes.
BASELINE_INK = {
    12: 219646, 13: 238924, 14: 259100,
    15: 259484, 16: 309064,
    18: 333500, 20: 370779,
}


def check_glyph_contrast(path: Path) -> None:
    """Pin the crisp-glyph rasterisation contract in the committed bitmaps."""
    text = path.read_text(encoding="utf-8")
    if not re.search(r"\.bpp = 1,", text):
        raise RuntimeError(f"{path.name}: expected a 1bpp PLAIN font")
    m = GLYPH_BITMAP_RE.search(text)
    if not m:
        raise RuntimeError(f"{path.name}: cannot find glyph_bitmap")
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    ink = sum(byte.bit_count() for byte in data)
    if ink == 0:
        raise RuntimeError(f"{path.name}: no ink pixels at all")

    # Guard the OTHER failure mode: too few ink pixels (skeletal Hanzi).
    m_size = re.search(r"_(\d+)\.c$", path.name)
    if m_size is None:
        raise RuntimeError(f"{path.name}: cannot determine font size")
    size = int(m_size.group(1))
    floor = int(BASELINE_INK[size] * MIN_INK_RATIO)
    if ink < floor:
        raise RuntimeError(
            f"{path.name}: only {ink} solid ink pixels, need at least "
            f"{floor} ({MIN_INK_RATIO:.0%} of baseline); thin 12 px strokes "
            "are being deleted, regenerate and check the skeleton"
        )


def check_generator_contract(sizes: tuple[int, ...]) -> None:
    """Pin the rasterisation strategy, not just the emitted bitmaps."""
    text = GENERATOR.read_text(encoding="utf-8")
    m = STROKE_CORE_THRESHOLD_RE.search(text)
    if not m:
        raise RuntimeError("tools/gen_reader_font.py: STROKE_CORE_THRESHOLD not found")
    t = int(m.group(1))
    if not (CORE_MIN <= t <= CORE_MAX):
        raise RuntimeError(
            f"STROKE_CORE_THRESHOLD={t} outside {CORE_MIN}..{CORE_MAX}: a low "
            "value reintroduces phase-sensitive edge pixels for ASCII/punctuation"
        )
    if not HINTED_MONO_RE.search(text):
        raise RuntimeError(
            "tools/gen_reader_font.py: hinted monochrome skeleton "
            '(getmask2(ch, mode="1")) is missing; ASCII/punctuation stems will '
            "drift with the antialiasing phase again"
        )
    # Squeezing the square strike into Noto's tight bbox once distorted every
    # Hanzi by a different factor and deleted 一's only stem. Pin the fix.
    if "def scale_square(" in text:
        raise RuntimeError(
            "tools/gen_reader_font.py: square strikes are being rescaled into "
            "Noto's tight glyph box again; characters will sit off the baseline "
            "and sparse stems will disappear")
    if "baseline_row - dst" not in text:
        raise RuntimeError(
            "tools/gen_reader_font.py: native strikes no longer place every "
            "Hanzi on one shared baseline (ofs_y = baseline_row - box_size)")

    # 17-20 px must come from a deterministic upscale of the 16 px strike,
    # not from the phase-sensitive Noto skeleton + grayscale core. The old
    # path gave horizontal stems 2 px and vertical ones 1 px and grew 2x2
    # black blocks from 11 to 26 per glyph ("some places thick, some thin").
    for needle in ("def upscale_strike(", "def fill_inner_corners(",
                   "HARMONIZE_MIN_SIZE = 16"):
        if needle not in text:
            raise RuntimeError(
                f"tools/gen_reader_font.py: {needle!r} is missing; >=16 px Hanzi "
                "would regress to pale/uneven strokes or the old random core")
    smap = re.search(r"BITMAP_STRIKE_FOR_SIZE\s*=\s*\{([^}]*)\}", text)
    if not smap:
        raise RuntimeError("tools/gen_reader_font.py: BITMAP_STRIKE_FOR_SIZE missing")
    pairs = dict((int(k), int(v)) for k, v in
                 re.findall(r"(\d+)\s*:\s*(\d+)", smap.group(1)))
    for size in sizes:
        if size not in pairs:
            raise RuntimeError(
                f"tools/gen_reader_font.py: {size}px has no native strike; "
                "it would fall back to the phase-sensitive vector path")
    # 12-15 px must stay frozen: no weighting, no rescale.
    for size in (12, 13, 14, 15):
        if pairs[size] not in (12, 13, 14):
            raise RuntimeError(
                f"tools/gen_reader_font.py: {size}px maps to a {pairs[size]}px "
                "strike; the frozen small sizes must use their native box")


STRIKES_FILE = FONT_DIR / "cjk_strikes.txt"
# Every selectable size is strike-based: 12-14 native, 15 = 14 px strike,
# 16 native, 18/20 = deterministic upscale of the 16 px strike.
NATIVE_SIZES = (12, 13, 14, 15, 16, 18, 20)


def check_native_strikes(inventory: set[int], sizes: tuple[int, ...]) -> None:
    """Pin the small-size dot-matrix source and its full Hanzi coverage.

    Scaling a vector bitmap down to 12 px is what clumped dense characters
    into black blocks. If a strike is missing, a glyph silently falls back to
    that blurrier path, so every Hanzi must be present in each native strike.
    """
    if not STRIKES_FILE.is_file():
        raise RuntimeError(f"missing native bitmap strikes: {STRIKES_FILE.name}")
    sections: dict[int, set[int]] = {}
    glyph_bits: dict[int, dict[int, tuple[int, int, str]]] = {}
    cur: int | None = None
    for line in STRIKES_FILE.read_text(encoding="utf-8").splitlines():
        if line.startswith("==STRIKE"):
            m = re.search(r"size=(\d+)", line)
            if not m:
                raise RuntimeError("cjk_strikes.txt: malformed ==STRIKE header")
            cur = int(m.group(1))
            sections[cur] = set()
        elif line.startswith("U+"):
            m = re.match(r"U\+([0-9A-Fa-f]{4,6})\s+(\d+)\s+(\d+)\s+([0-9A-Fa-f]+)\s*$", line)
            if not m or cur is None:
                raise RuntimeError(f"cjk_strikes.txt: malformed glyph line {line[:24]!r}")
            cp = int(m.group(1), 16)
            sections[cur].add(cp)
            glyph_bits.setdefault(cur, {})[cp] = (
                int(m.group(2)), int(m.group(3)), m.group(4))
    hanzi = {cp for cp in inventory if 0x4E00 <= cp <= 0x9FFF}
    for strike_size in (12, 13, 14, 16):
        if strike_size not in sections:
            raise RuntimeError(f"cjk_strikes.txt: missing {strike_size}px strike")
        absent = hanzi - sections[strike_size]
        if absent:
            raise RuntimeError(
                f"cjk_strikes.txt: {strike_size}px strike misses {len(absent)} Hanzi, e.g. "
                + " ".join(f"U+{cp:04X}" for cp in sorted(absent)[:8]))

        # Structural regression. A 13x13 strike is 169 bits, not a multiple of
        # 4: padding the hex on the LEFT once shifted every row by 3 px and
        # split the horizontal stem of 一 into two fragments. 一 must still
        # contain a near-full-width horizontal row in every strike.
        if 0x4E00 in glyph_bits.get(strike_size, {}):
            w, h, hexbits = glyph_bits[strike_size][0x4E00]
            bits = bin(int(hexbits, 16))[2:].zfill(len(hexbits) * 4)[: w * h]
            widest = max(bits[r * w:(r + 1) * w].count("1") for r in range(h))
            if widest < w - 2:
                raise RuntimeError(
                    f"cjk_strikes.txt: {strike_size}px strike U+4E00 一 has a "
                    f"{widest}/{w}px widest stem; bitmap rows are misaligned "
                    "(check hex padding in tools/extract_cjk_strikes.py)")

    gen = GENERATOR.read_text(encoding="utf-8")
    for needle in ("BITMAP_STRIKE_FOR_SIZE", "def load_strikes(",
                   "def strike_glyph("):
        if needle not in gen:
            raise RuntimeError(
                f"gen_reader_font.py: native strike path {needle!r} is missing; "
                "small Hanzi would regress to the scaled vector bitmap")

    for size in sizes:
        path = FONT_DIR / f"lv_font_reader_{size}.c"
        head = "\n".join(path.read_text(encoding="utf-8").splitlines()[:10])
        want = "dot-matrix strike" if size in NATIVE_SIZES else "Noto hinted monochrome skeleton"
        if want not in head:
            raise RuntimeError(
                f"{path.name}: header does not record the expected rasterisation "
                f"({want}); regenerate with tools/gen_reader_font.py")


def check_native_geometry(path: Path) -> None:
    """Pin the two symptoms the reader reported: off-baseline text and 一 vanishing.

    In a native-strike font every Hanzi must use the SAME square box and ofs_y
    (one shared baseline). 一 (U+4E00) must still contain a near-full-width
    horizontal row: squeezing the strike into Noto's tight 12x6 box used to
    skip that row entirely, leaving 2 pixels instead of a stem.
    """
    size = int(re.search(r"_(\d+)\.c$", path.name).group(1))
    if size not in NATIVE_SIZES:
        return
    text = path.read_text(encoding="utf-8")

    cmaps = re.search(r"cmaps\[\] = \{(.*?)\};", text, re.S)
    dsc = re.search(r"glyph_dsc\[\] = \{(.*?)\};", text, re.S)
    m = GLYPH_BITMAP_RE.search(text)
    if not cmaps or not dsc or not m:
        raise RuntimeError(f"{path.name}: cannot find cmaps / glyph_dsc / bitmap")

    starts = [int(x, 0) for x in re.findall(r"\.range_start = (\d+)", cmaps.group(1))]
    gids = [int(x) for x in re.findall(r"\.glyph_id_start = (\d+)", cmaps.group(1))]
    cmap: dict[int, int] = {}
    ar = re.search(r"\.range_start = 32, \.range_length = (\d+),", text)
    if ar:
        cmap.update({cp: 1 + cp - 32 for cp in range(32, 32 + int(ar.group(1)))})
    for i in range(1, len(starts)):
        um = re.search(rf"unicode_list_cjk_{i - 1}\[\] = \{{(.*?)\}};", text, re.S)
        offs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", um.group(1))] if um else [0]
        for j, off in enumerate(offs):
            cmap[starts[i] + off] = gids[i] + j

    parsed = [tuple(map(int, r)) for r in re.findall(
        r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
        r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", dsc.group(1))]
    han = [(cp, parsed[gid]) for cp, gid in cmap.items()
           if 0x4E00 <= cp <= 0x9FFF and 0 < gid < len(parsed)]
    boxes = {(p[2], p[3], p[4], p[5]) for _, p in han}
    if len(boxes) != 1:
        raise RuntimeError(
            f"{path.name}: Hanzi use {len(boxes)} different glyph boxes / "
            "baselines; they will not sit on one line (regenerate with the "
            "native square strike box)")

    gid = cmap.get(0x4E00)
    if gid is None or gid >= len(parsed):
        raise RuntimeError(f"{path.name}: U+4E00 一 is missing from the font")
    _bi, _aw, bw, _bh, _ox, _oy = parsed[gid]
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    _, _, rows = _glyph_bitmap(gid, parsed, data)
    widest = max(map(sum, rows))
    if widest < bw - 2:
        raise RuntimeError(
            f"{path.name}: U+4E00 一 has only a {widest}/{bw}px widest stem; "
            "the horizontal stroke was cropped or the bitmap rows are shifted")


# ---------------------------------------------------------------------------
# 12-16 px freeze and >=16 px stroke balance
# ---------------------------------------------------------------------------
# Hash non-Latin bitmaps and metrics, Latin advances, and cmap geometry.
FROZEN_SIZES = (12, 13, 14, 15, 16, 18, 20)
FROZEN_DATA_SHA256 = {
    12: "921075352d003b1a4d0a3402d3af015b00ed5dcbc97faa58783e9f64c608367d",
    13: "447d8311f86bbc1d9919810bd88486dbc45f5114cd46db39ec819b230136212e",
    14: "230653e7ba0f413a81edfcc31be37a95f37d6b8d6e2a562425933704affa6bd9",
    15: "4474c8fffeddbe4b1c0dd00c3a2db6fdea64045b35511eb79a91ae4dc3cceed2",
    16: "c98ae1ec3910574439355b7b2d6b383dfdee12e315aa25d6a89378fdf8c975fe",
    18: "a24dfc067b62f8b1fb6bf814fbf79d022c3c4330af83728440fa03a0344719eb",
    20: "65944540ae9bb3f48391afe2a4793e8d0ad4c89f7830318eaa665886f72a769c",
}


LATIN_GIDS = {cp - 31 for lo, hi in ((ord("A"), ord("Z")), (ord("a"), ord("z")))
              for cp in range(lo, hi + 1)}
LATIN_LIGHT_INK = {12: 946, 13: 1028, 14: 1149, 15: 1160, 16: 1280,
                   18: 1493, 20: 1756}


def _font_data_blob(text: str) -> bytes:
    bitmap = bytes(int(x, 16) for x in re.findall(
        r"0x([0-9A-Fa-f]{2})", GLYPH_BITMAP_RE.search(text).group(1)))
    dsc = re.search(r"glyph_dsc\[\] = \{(.*?)\};", text, re.S)
    glyphs = [tuple(map(int, row)) for row in re.findall(
        r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
        r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", dsc.group(1))]
    cmaps = re.search(r"cmaps\[\] = \{(.*?)\};", text, re.S)
    data = bytearray(cmaps.group(1).encode())
    codepoints = sorted(int(cp, 16) for cp in INVENTORY_RE.findall(
        CHARSET_FILE.read_text(encoding="utf-8")))
    bpp = int(re.search(r"\.bpp = (\d+),", text).group(1))
    for gid, (offset, advance, w, h, x, y) in enumerate(glyphs):
        if gid and unicodedata.category(chr(codepoints[gid - 1])).startswith("P"):
            data.extend(f"{advance};".encode())
        else:
            data.extend(f"{advance},{w},{h},{x},{y};".encode())
            mask = (1 << bpp) - 1
            for i in range(w * h):
                bit = i * bpp
                level = (bitmap[offset + bit // 8] >> (8 - bpp - bit % 8)) & mask
                data.append(int(bool(level)))
    return bytes(data)


def check_frozen_small_sizes() -> None:
    """Pin approved glyphs and advances, allowing punctuation to change."""
    for size in FROZEN_SIZES:
        path = FONT_DIR / f"lv_font_reader_{size}.c"
        digest = hashlib.sha256(
            _font_data_blob(path.read_text(encoding="utf-8"))
        ).hexdigest()
        if digest != FROZEN_DATA_SHA256[size]:
            raise RuntimeError(
                f"{path.name}: frozen {size}px glyph data or advances changed (SHA-256 "
                f"{digest[:12]}...)")


def check_ascii_advances(sizes: tuple[int, ...]) -> None:
    source = (FONT_DIR / "reader_ascii_advance.h").read_text(encoding="utf-8")
    table = source.split("reader_ascii_advance[][95] = {", 1)[1]
    actual = [int(value) for value in re.findall(r"\d+", table)]
    expected = []
    for size in sizes:
        text = (FONT_DIR / f"lv_font_reader_{size}.c").read_text(encoding="utf-8")
        expected.extend(int(advance) for advance in re.findall(r"\.adv_w = (\d+)", text)[1:96])
    if actual != expected:
        raise RuntimeError("reader_ascii_advance.h does not match the shipped glyph widths")


def check_strike_expansion() -> None:
    from runpy import run_path

    generator = run_path(str(ROOT / "tools" / "gen_reader_font.py"))
    upscale = generator["upscale_strike"]
    stretch = generator["stretch_strike_rows"]
    corner = [[0] * 16 for _ in range(16)]
    corner[7][6:8] = [1, 1]
    corner[8][7] = 1
    if stretch(corner, 16, 17)[8] != corner[8]:
        raise RuntimeError("Expansion added ink alongside a connected corner")
    for size in (18, 20):
        for stem in range(16):
            horizontal = [[int(r == stem)] * 16 for r in range(16)]
            vertical = [list(row) for row in zip(*horizontal)]
            for strike in (horizontal, vertical):
                expanded = upscale(strike, 16, size)
                if sum(map(sum, expanded)) != size:
                    raise RuntimeError(f"{size}px: 1px stem at {stem} changed thickness")
        for slope in (-1, 1):
            diagonal = [[int(c == (r if slope == 1 else 15 - r))
                         for c in range(16)] for r in range(16)]
            expanded = upscale(diagonal, 16, size)
            columns = [{c for c, pixel in enumerate(row) if pixel} for row in expanded]
            if not all(columns) or any(
                not any(abs(a - b) <= 1 for a in upper for b in lower)
                for upper, lower in zip(columns, columns[1:])
            ):
                raise RuntimeError(f"{size}px: expansion broke a diagonal stroke")


# Sampled 2x2 black-block density and vertical/horizontal 2-pixel run ratio.
# Large strikes extend the unweighted source, so 17 px retains slightly more
# of the native Song face's vertical weight.
BLOCK_CEILING = {16: 4.0, 18: 1.0, 20: 1.0}
BALANCE_MIN, BALANCE_MAX = 0.65, 1.45


def _runs(seq) -> list[int]:
    out: list[int] = []
    n = 0
    for x in seq:
        if x:
            n += 1
        elif n:
            out.append(n)
            n = 0
    if n:
        out.append(n)
    return out


def _glyph_bitmap(gid: int, parsed, data: bytes):
    if gid >= len(parsed):
        return None
    bi, _aw, bw, bh, _ox, _oy = parsed[gid]
    if bw == 0 or bh == 0:
        return None
    raw = data[bi:bi + (bw * bh + 7) // 8]
    vals = [(byte >> shift) & 1 for byte in raw for shift in range(7, -1, -1)]
    vals = vals[:bw * bh]
    return bw, bh, [vals[r * bw:(r + 1) * bw] for r in range(bh)]


def check_stroke_balance(path: Path) -> None:
    """Check the Latin alphabet and sampled Hanzi stroke weight."""
    size = int(re.search(r"_(\d+)\.c$", path.name).group(1))
    text = path.read_text(encoding="utf-8")
    cmaps = re.search(r"cmaps\[\] = \{(.*?)\};", text, re.S)
    dsc = re.search(r"glyph_dsc\[\] = \{(.*?)\};", text, re.S)
    m = GLYPH_BITMAP_RE.search(text)
    if not cmaps or not dsc or not m:
        raise RuntimeError(f"{path.name}: cannot find cmaps / glyph_dsc / bitmap")

    starts = [int(x, 0) for x in re.findall(r"\.range_start = (\d+)", cmaps.group(1))]
    gids = [int(x) for x in re.findall(r"\.glyph_id_start = (\d+)", cmaps.group(1))]
    cmap: dict[int, int] = {}
    for i in range(1, len(starts)):
        um = re.search(rf"unicode_list_cjk_{i - 1}\[\] = \{{(.*?)\}};", text, re.S)
        offs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", um.group(1))] if um else [0]
        for j, off in enumerate(offs):
            cmap[starts[i] + off] = gids[i] + j
    parsed = [tuple(map(int, r)) for r in re.findall(
        r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
        r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", dsc.group(1))]
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))

    latin_ink = 0
    for gid in LATIN_GIDS:
        glyph = _glyph_bitmap(gid, parsed, data)
        ink = sum(map(sum, glyph[2])) if glyph else 0
        if not ink:
            raise RuntimeError(f"{path.name}: Latin letter {chr(gid + 31)} lost its ink")
        latin_ink += ink
    expected_ink = LATIN_LIGHT_INK[size]
    if not (0.95 * expected_ink <= latin_ink <= 1.05 * expected_ink):
        raise RuntimeError(f"{path.name}: Latin stroke weight differs from the Light face")
    if size < 16:
        return

    han_gids = [cmap[cp] for cp in sorted(cmap)
                if 0x4E00 <= cp <= 0x9FFF and cp in cmap]
    sample = han_gids[::8]
    if not sample:
        raise RuntimeError(f"{path.name}: no Hanzi to sample")
    blocks = h2 = v2 = 0
    for gid in sample:
        g = _glyph_bitmap(gid, parsed, data)
        if not g:
            continue
        bw, bh, rows = g
        blocks += sum(1 for r in range(bh - 1) for c in range(bw - 1)
                      if rows[r][c] and rows[r][c + 1]
                      and rows[r + 1][c] and rows[r + 1][c + 1])
        h2 += sum(1 for row in rows for x in _runs(row) if x == 2)
        v2 += sum(1 for c in range(bw)
                  for x in _runs([rows[r][c] for r in range(bh)]) if x == 2)
    n = len(sample)
    density = blocks / n
    ratio = v2 / max(1, h2)
    if density > BLOCK_CEILING[size]:
        raise RuntimeError(
            f"{path.name}: {density:.1f} 2x2 black blocks/glyph exceeds "
            f"{BLOCK_CEILING[size]}; phase-sensitive 2 px strokes returned "
            "('some places thicker, some thinner')")
    if not (BALANCE_MIN <= ratio <= BALANCE_MAX):
        raise RuntimeError(
            f"{path.name}: vertical/horizontal 2-px stroke ratio {ratio:.2f} "
            f"outside {BALANCE_MIN}..{BALANCE_MAX}; stems are uneven")


def check_charset_header(inventory: set[int]) -> None:
    if not CHARSET_HEADER.is_file():
        raise RuntimeError(f"missing generated charset header: {CHARSET_HEADER}")
    text = CHARSET_HEADER.read_text(encoding="utf-8")
    m = re.search(r"reader_charset_codepoints\[[^\]]*\] = \{(.*?)\};", text, re.S)
    if not m:
        raise RuntimeError(f"{CHARSET_HEADER.name}: cannot find code-point table")
    header_cps = {int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{4,6})", m.group(1))}
    if header_cps != inventory:
        raise RuntimeError(
            f"{CHARSET_HEADER.name}: table has {len(header_cps)} code points, "
            f"charset.txt has {len(inventory)}; regenerate with tools/gen_reader_font.py"
        )
    count = re.search(r"#define READER_CHARSET_COUNT\s+(\d+)", text)
    if not count or int(count.group(1)) != len(inventory):
        raise RuntimeError(f"{CHARSET_HEADER.name}: READER_CHARSET_COUNT mismatch")


def check_fallback_coverage() -> int:
    text = (FONT_DIR / "reader_fallback_data.h").read_text(encoding="utf-8")
    match = re.search(r"fallback_codepoints\[\] = \{(.*?)\};", text, re.S)
    if not match:
        raise RuntimeError("missing fallback code point array")
    actual = [int(value, 16) for value in re.findall(r"0x([0-9A-F]{4})", match.group(1))]
    expected = []
    for first in range(0xD8, 0xF8):
        for second in range(0xA1, 0xFF):
            try:
                expected.append(ord(bytes((first, second)).decode("gb2312")))
            except UnicodeDecodeError:
                continue
    if actual != sorted(expected) or len(actual) != 3008:
        raise RuntimeError("fallback does not cover exactly GB2312 level 2")
    rows = re.findall(r"\{((?:0x[0-9A-F]{4},? ?)+)\}, // U\+([0-9A-F]{4})", text)
    if [int(cp, 16) for _, cp in rows] != actual:
        raise RuntimeError("fallback glyph order differs from its lookup table")
    for bitmap, cp in rows:
        values = [int(value, 16) for value in re.findall(r"0x([0-9A-F]{4})", bitmap)]
        if len(values) != 16 or not any(values):
            raise RuntimeError(f"fallback U+{cp}: invalid 16 px strike")
    return len(actual)


def main() -> int:
    errors: list[str] = []
    try:
        inventory = read_inventory()
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    if not inventory:
        print("ERROR: empty character inventory", file=sys.stderr)
        return 1

    # Negative control: the coverage check must be able to fail.
    if NEGATIVE_CONTROL in inventory:
        errors.append(f"negative control U+{NEGATIVE_CONTROL:04X} unexpectedly present")
    if not missing({chr(NEGATIVE_CONTROL)}, inventory):
        errors.append("coverage check cannot detect a known-missing code point")

    absent = missing(source_chars(), inventory)
    if absent:
        errors.append(
            "characters used in main/ string literals are missing from the font: "
            + " ".join(f"U+{cp:04X} {chr(cp)}" for cp in absent[:40])
        )

    try:
        sizes = font_sizes()
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    for name in stale_font_files(sizes):
        errors.append(f"{name} is not referenced by any selectable size")

    fonts = font_files(sizes)
    for font in fonts:
        try:
            check_font_file(font, inventory)
            check_glyph_contrast(font)
            check_native_geometry(font)
            check_stroke_balance(font)
        except RuntimeError as exc:
            errors.append(str(exc))
    try:
        check_generator_contract(sizes)
        check_strike_expansion()
    except RuntimeError as exc:
        errors.append(str(exc))
    try:
        check_native_strikes(inventory, sizes)
    except RuntimeError as exc:
        errors.append(str(exc))
    try:
        check_frozen_small_sizes()
        check_ascii_advances(sizes)
    except RuntimeError as exc:
        errors.append(str(exc))
    try:
        check_charset_header(inventory)
    except RuntimeError as exc:
        errors.append(str(exc))

    fallback_count = 0
    try:
        fallback_count = check_fallback_coverage()
    except RuntimeError as exc:
        errors.append(str(exc))

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print("Regenerate fonts with: python3 tools/gen_reader_font.py", file=sys.stderr)
        return 1
    print(f"reader_font_coverage: PASS ({len(inventory)} code points, "
          f"{len(fonts)} crisp 1bpp fonts, {sizes[0]}..{sizes[-1]} px; "
          f"{fallback_count} fallback glyphs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
