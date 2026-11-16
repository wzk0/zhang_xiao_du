#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local dead_strip_flag

    # Apple ld does not accept --gc-sections; -dead_strip is the equivalent.
    dead_strip_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        dead_strip_flag="-Wl,-dead_strip"
    fi

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/battery_stubs -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_battery.c -o "${test_dir}/test_bsp_battery"
    "${test_dir}/test_bsp_battery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${dead_strip_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    # eBook reader: pure-logic text/progress tests and the Chinese font
    # inventory gate. These must run before the firmware build so a missing
    # glyph or a layout regression cannot reach the device image.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_reader_text.c main/reader_text.c \
        -o "${test_dir}/test_reader_text"
    "${test_dir}/test_reader_text"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_reader_progress.c main/reader_progress.c \
        -o "${test_dir}/test_reader_progress"
    "${test_dir}/test_reader_progress"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_reader_bookmark.c main/reader_bookmark.c main/reader_progress.c \
        -o "${test_dir}/test_reader_bookmark"
    "${test_dir}/test_reader_bookmark"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_reader_layout.c main/reader_layout.c main/reader_text.c \
        -o "${test_dir}/test_reader_layout"
    "${test_dir}/test_reader_layout"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_reader_storage.c main/reader_storage.c \
        -o "${test_dir}/test_reader_storage"
    "${test_dir}/test_reader_storage"
    # zdx decoder: the exact C the firmware uses, decoded against fixtures
    # produced by tools/zdx_codec.py (same raw-deflate settings as pako).
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_reader_zdx.py
    # Whole state machine under in-memory stubs: navigation, finished-book
    # back-step (the stack-overflow reboot regression), bookmark labels and
    # the long-press delete / storage flows.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/reader_app_stubs -Imain -Icomponents/uzlib \
        tests/test_reader_app_navigation.c \
        main/reader_text.c main/reader_layout.c main/reader_storage.c \
        main/reader_progress.c main/reader_bookmark.c \
        main/builtin_books.c main/builtin_books_data.c main/reader_zdx.c \
        components/uzlib/tinflate.c components/uzlib/crc32.c \
        components/uzlib/adler32.c \
        -o "${test_dir}/test_reader_app_navigation"
    "${test_dir}/test_reader_app_navigation"
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_reader_font_coverage.py
    PYTHONDONTWRITEBYTECODE=1 python3 tools/gen_reader_font.py --check
    PYTHONDONTWRITEBYTECODE=1 python3 tools/gen_builtin_books.py --check
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_battery_profile.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    # The merged image alone is not enough: tools/verify_firmware.py and
    # `idf.py flash` read the SPLIT images from build/. Copy the exact files
    # that were just merged so the deliverable directory stays self-consistent
    # (a quick incremental build in build/ must not leave stale split images
    # that disagree with the verified full binary).
    mkdir -p "${repo_root}/build/bootloader" "${repo_root}/build/partition_table"
    for img in "bootloader/bootloader.bin" \
               "partition_table/partition-table.bin" \
               "FoloToy-AI-Passport.bin" "flash_args"; do
        install -m 0644 "${validation_build_dir}/${img}" \
            "${repo_root}/build/${img}"
    done
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
