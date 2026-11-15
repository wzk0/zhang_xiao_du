<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, sound effects, and battery profiles, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### Reader fonts (`fonts/lv_font_reader_12.c` … `_20.c`)

Nine 2bpp PLAIN bitmap fonts covering 3,987 code points. **Every size draws
Hanzi from dot-matrix strikes**; only ASCII, punctuation and UI symbols use
Noto Sans CJK SC. Advance width always comes from Noto, and pagination and
bookmarks are byte offsets, so switching size never changes layout:

| Range | Source | License |
| --- | --- | --- |
| 12–15 px Hanzi (frozen) | WenQuanYi bitmap-song **native dot-matrix strikes** (12/13/14 px; 15 px reuses the 14 px strike unscaled, unweighted) | GPL-2-with-font-embedding-exception — [`fonts/LICENSE.wqy-bitmapfont.txt`](fonts/LICENSE.wqy-bitmapfont.txt) |
| 16 px Hanzi | native 16x16 strike + **inner-corner fill** (+8% ink; vertical/horizontal 2px-run ratio 1.89 -> 0.85) | same as above |
| 17–20 px Hanzi | 16x16 strike **deterministically upscaled** to the target size + inner-corner fill | same as above |
| ASCII, punctuation, UI symbols (all sizes) | Noto Sans CJK SC FreeType hinted monochrome skeleton + stroke core | SIL OFL 1.1 (source TTC not redistributed) |

Hanzi always use the strike's **native square em box** (or its scaled square
box): every Hanzi shares one box and one baseline (`ofs_y = baseline_row -
box_size`), stems are 1 px, and sparse glyphs such as U+4E00 keep their full
horizontal stroke. An earlier build squeezed the square strike into Noto's
tight bbox, distorting each character by a different factor and cropping that
stroke; the coverage gate now pins the fix. Square boxes are smaller than
Noto's tall tight boxes, so the nine bitmaps total ~2.19 MiB.

**Why 17–20 px changed:** the old Noto skeleton hinted CJK horizontal stems to
2 px while leaving vertical ones at 1 px, then OR-ed in a grayscale core at
random single-pixel phases, so a single line mixed thick and thin strokes and
2x2 black blocks grew from 11 to 26 per glyph across 17–20 px. Deterministic
nearest-neighbour upscaling lands the extra rows/columns at even intervals,
bringing the stroke ratio to ~1.0 and cutting those blocks by 37–83%. The old
build also jumped +28% in ink from 16 px to 17 px, which made 16 px look pale
by contrast; the jump is now +4.3%, and 16 px itself gained 8% ink.

The 12–15 px sizes were called perfect: their `glyph_bitmap`, `glyph_dsc` and
`cmaps` arrays are pinned by SHA-256 in the coverage gate, so no pixel or
geometry can drift. 16–20 px are additionally bounded by a sampled 2x2 block
density and a horizontal/vertical stroke-balance ratio. The extracted strike table is
[`fonts/cjk_strikes.txt`](fonts/cjk_strikes.txt); the original PCF/BDF files
are not committed. See `tools/extract_cjk_strikes.py` for regeneration, and
`tools/gen_reader_font.py` / `tests/test_reader_font_coverage.py` for the
build and coverage gates.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

## Battery profiles

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
contains the supplied CW2017 profile for the Wanli New Energy 500 mAh cell
(profile2, XIL40X, 2026-10-20). It is an unchanged copy of the project-supplied
file, retained at the maintainer's request for this firmware update. Its
original filename is recorded in the paired Chinese document. No separate
license was included in the supplied file.

The format is 80 lines of two hexadecimal digits, one byte per line in register
order. The BSP embeds those bytes in `components/bsp/src/bsp_battery.c`; the
host gate checks the source file and initializer for an exact match. Text-file
SHA-256: `7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`.
Decoded 80-byte SHA-256:
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`.
This is a fuel-gauge profile, not a charging configuration or a generic profile
for every 500 mAh cell; see the [hardware guide](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md#9-cw2017-fuel-gauge).
