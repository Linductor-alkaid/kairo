#!/bin/bash
# 在 Android 设备/模拟器上运行 kairo standalone 测试。
#
# 用法：
#   scripts/run_android_tests.sh --serial emulator-5554 --test-dir build-android/arm64-v8a/static/tests
#   scripts/run_android_tests.sh --serial DEVICE_SERIAL --test-dir ... --soak-seconds 600
#
# 也可以通过 ANDROID_SERIAL 提供 serial；未指定 binaries 时运行 test-dir 下
# android_smoke、test_stop_token_compat、test_executor_manager_android_defaults。

set -e

SERIAL="${ANDROID_SERIAL:-}"
TEST_DIR=""
SOAK_SECONDS="${KAIRO_ANDROID_SOAK_SECONDS:-0}"
DEVICE_DIR="/data/local/tmp/kairo-tests"
CLEANUP="${ANDROID_TEST_CLEANUP:-true}"
BINARIES=()

usage() {
    echo "Usage: $0 --test-dir <dir> [--serial <serial>] [--soak-seconds <seconds>] [binary ...]"
    exit 1
}

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
        --cleanup)
            CLEANUP="$2"
            shift 2
            ;;
        --)
            shift
            BINARIES+=("$@")
            break
            ;;
        -*)
            echo "Unknown option: $1"
            usage
            ;;
        *)
            BINARIES+=("$1")
            shift
            ;;
    esac
done

if [[ -z "$TEST_DIR" ]]; then
    echo "Error: --test-dir is required"
    usage
fi

if ! command -v adb >/dev/null 2>&1; then
    echo "Error: adb not found"
    exit 1
fi

ADB=(adb)
if [[ -n "$SERIAL" ]]; then
    ADB+=(-s "$SERIAL")
fi

"${ADB[@]}" get-state >/dev/null

if [[ ${#BINARIES[@]} -eq 0 ]]; then
    for name in \
            android_smoke \
            test_stop_token_compat \
            test_executor_manager_android_defaults \
            test_android_lockfree_queue_core \
            test_multithread_mpsc \
            test_lockfree_worker_queue_concurrent_steal; do
        [[ -x "$TEST_DIR/$name" ]] && BINARIES+=("$TEST_DIR/$name")
    done
fi

if [[ ${#BINARIES[@]} -eq 0 ]]; then
    echo "Error: no test binaries found in $TEST_DIR"
    exit 1
fi

"${ADB[@]}" shell mkdir -p "$DEVICE_DIR"
trap 'if [[ "$CLEANUP" == "true" ]]; then "${ADB[@]}" shell rm -rf "$DEVICE_DIR" >/dev/null 2>&1 || true; fi' EXIT

failed=0
for binary in "${BINARIES[@]}"; do
    [[ -x "$binary" ]] || {
        echo "Error: $binary is not executable"
        failed=1
        continue
    }
    name="$(basename "$binary")"
    remote="$DEVICE_DIR/$name"
    echo "==> push and run $name"
    "${ADB[@]}" push "$binary" "$remote" >/dev/null
    "${ADB[@]}" shell chmod 755 "$remote"
    set +e
    output="$("${ADB[@]}" shell "KAIRO_ANDROID_SOAK_SECONDS=$SOAK_SECONDS $remote" 2>&1)"
    status=$?
    set -e
    echo "$output"
    if [[ $status -eq 0 ]]; then
        echo "<== PASS $name"
    else
        echo "<== FAIL $name (exit $status)"
        failed=1
    fi
done

exit "$failed"
