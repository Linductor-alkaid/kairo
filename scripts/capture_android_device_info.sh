#!/bin/bash
# 采集 Android 设备信息并运行 standalone 测试，输出可供 A3 结果文档使用。
#
# 用法：
#   scripts/capture_android_device_info.sh --test-dir <dir> [--serial <serial>] [--soak-seconds 600]

set -e

SERIAL="${ANDROID_SERIAL:-}"
TEST_DIR=""
SOAK_SECONDS="${KAIRO_ANDROID_SOAK_SECONDS:-600}"
EXTRA_RUN_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --serial)
            SERIAL="$2"
            shift 2
            ;;
        --test-dir)
            TEST_DIR="$2"
            shift 2
            ;;
        --soak-seconds)
            SOAK_SECONDS="$2"
            shift 2
            ;;
        *)
            EXTRA_RUN_ARGS+=("$1")
            shift
            ;;
    esac
done

if ! command -v adb >/dev/null 2>&1; then
    echo "Error: adb not found"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ADB=(adb)
if [[ -n "$SERIAL" ]]; then
    ADB+=(-s "$SERIAL")
fi
"${ADB[@]}" get-state >/dev/null

echo "== Device information =="
"${ADB[@]}" shell getprop ro.product.model
"${ADB[@]}" shell getprop ro.product.board
"${ADB[@]}" shell getprop ro.build.version.release
"${ADB[@]}" shell getprop ro.build.version.sdk
"${ADB[@]}" shell uname -m
"${ADB[@]}" shell uname -r
echo "== CPU parts =="
"${ADB[@]}" shell "grep -E 'processor|CPU part|CPU implementer' /proc/cpuinfo | head -40" || true

if [[ -n "$TEST_DIR" ]]; then
    RUN_ARGS=(--test-dir "$TEST_DIR")
    if [[ -n "$SERIAL" ]]; then
        RUN_ARGS+=(--serial "$SERIAL")
    fi
    RUN_ARGS+=(--soak-seconds "$SOAK_SECONDS")
    echo "== Running standalone tests =="
    "$SCRIPT_DIR/run_android_tests.sh" "${RUN_ARGS[@]}" "${EXTRA_RUN_ARGS[@]}"
fi
