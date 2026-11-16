<p align="right">
  <a href="AGENTS.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Repository Guidelines for AI Agents

This file and the root `README.md` are the only maintained documents. Read this
file first; for everything else the code is the reference: headers,
neighbouring implementations, and `components/bsp/include/bsp_pins.h`.

## Required AI skills

The following five skills are required for AI-assisted development in this repository:
`passport-develop`, `passport-setup`, `passport-build`, `passport-device-test`, and
`passport-debug`. Their maintained sources are under `skills/`.

Before starting development, the AI must check that all five are installed and
available in its current environment. If any are missing, the AI must choose an
appropriate installation method for its tool and environment, perform the
installation, and verify availability itself. Do not wait for the user to request
installation or make the user choose the method or run installation commands.
No particular script, installation directory, or AI client is mandatory.

Respect the environment's approval requirements and preserve existing skills
and user configuration. If permissions, conflicts, or platform limitations prevent
installation, explain the blocker and request only the necessary user action;
do not claim installation succeeded. Having all five available does not mean
loading or invoking all five for every task: use only the matching skills, and
retain the separate authorization requirements for flashing, Git writes, and publishing.

## Project and safety baseline

- Target: ESP32-C3, 8 MB Flash, no PSRAM, ESP-IDF 5.5.3.
- This checkout is an eBook reader (`main/reader_*`); `main/demo_*.c` are the
  BSP demonstration pages, not the product.
- Keep the default partition table minimal: NVS, PHY data, and one factory
  application spanning the rest of the 8 MB Flash. User firmware may deliberately
  change this layout; validate the resulting table and do not turn
  product-specific partitions into mandatory template contracts.
- Hardware facts follow this priority: product specifications and measured
  results → `components/bsp/include/bsp_pins.h` → BSP headers and implementation
  → this file → `README.md` and demo code. If a task needs a hardware detail
  none of these defines, ask the user instead of guessing.
- Reusable board logic belongs in `components/bsp`; pages, state machines,
  animations, and application tasks belong in `main`.
- Derivative applications must redesign and implement their own UI. Reusing the
  demo test menu, screens, or visual shell is prohibited; renaming, recoloring,
  or adding a feature to that shell is not a redesign. BSP APIs and non-UI logic
  remain reusable.
- LVGL is not thread-safe. Code outside the LVGL task must hold `bsp_lvgl_lock()`
  while accessing LVGL objects.
- Chinese UI text needs real glyph coverage. The Montserrat fonts have no Chinese
  glyphs; UTF-8 source and a successful build prove nothing. Fonts are generated
  into `assets/fonts/lv_font_reader_*.c` by `tools/gen_reader_font.py` from
  `assets/fonts/charset.txt`, and `tests/test_reader_font_coverage.py` plus
  `tools/gen_reader_font.py --check` gate the result. Verify coverage, widget
  font selection, and on-device rendering before claiming text works.
- Button callbacks must stay non-blocking. Audio, storage, networking, and other
  slow operations belong in worker tasks.
- A demo must stop every task, timer, callback, and event handler that can access
  its UI before deleting the screen.
- Keep testable state machines, protocols, timing, and layout calculations
  independent from ESP-IDF/LVGL and cover them with host tests.
- Never commit credentials, device QR secrets, private keys, personal data, or
  unsanitized logs.

## Documentation policy

- `README.md` and this file pair are the whole documentation set. Do not add a
  `docs/` tree, per-feature notes, or progress documents; put durable user-facing
  facts in `README.md` and durable engineering rules in this file.
- This pair is bilingual: English at `AGENTS.md`, Simplified Chinese at
  `AGENTS.zh_CN.md`, with reciprocal top language links, aligned sections, and no
  CJK prose in the English default.
- `README.md` may use any language and needs no pair.

## Task-specific context routing

| Task | Read before editing |
| --- | --- |
| Any code change | `git status --short --branch`, the affected header, and the nearest implementation |
| Application development or skill setup | `skills/README.md`, then only the matching skill |
| Pins, buses, display, audio, battery | `components/bsp/include/bsp_pins.h`, the matching BSP header and source |
| eBook reader state, layout, storage, transfer | `main/reader_*.h`, `main/reader_app.c`, `main/reader_ui.c` |
| Chinese UI text or fonts | `assets/fonts/charset.txt`, `assets/fonts/reader_charset.h`, `tools/gen_reader_font.py`, `tests/test_reader_font_coverage.py` |
| Wi-Fi, Bluetooth, low power | `main/demo_wifi.c`, `main/demo_ble.c`, `main/demo_low_power.c`, `components/bsp/include/bsp_button.h` |
| Build, tests, partitioning | `tools/validate.sh`, `sdkconfig.defaults`, `partitions.csv` |
| CI, release, flashed image | `.github/workflows/`, `tools/verify_firmware.py`, `tools/archive_firmware.py` |
| Product overview and usage | `README.md` |

## Required validation and delivery

Run the smallest relevant check while iterating, then run the complete gate before delivery:

```bash
./tools/validate.sh --static    # repository checks + host tests
./tools/validate.sh --firmware  # ESP-IDF build + merged-image verification
./tools/validate.sh             # complete gate
```

The complete gate requires an activated ESP-IDF 5.5.3 environment. Do not describe a successful build as hardware validation. Final delivery must report these fields separately:

```text
Build: PASS / FAIL / NOT RUN
Host tests: PASS / FAIL / NOT RUN
Device tests: PASS / FAIL / NOT RUN
Unverified: remaining board, instrument, or user checks
```

## Device testing and Git

- Flashing does not require backing up the firmware already on the device, and a
  Flash readback is never a prerequisite. It also does not authorize a full-chip
  erase or promise that user data survives.
- After each complete implementation of a user-requested firmware change,
  proactively ask whether to flash it for testing; do not wait for a release
  request. If no device is detected, ask the user to turn it on and connect it to
  a computer USB port with a data-capable cable. Detection alone is not consent:
  obtain approval before flashing.
- Create commits and push only when the user requests them or the active workflow
  explicitly requires them. This repository has no changelog; describe
  user-visible behavior, compatibility, and release-flow impact in the commit or
  pull-request body.