#!/bin/bash
# Android 交叉编译脚本
# 构建 kairo 静态库和/或共享库，并安装到统一输出目录。
#
# 用法：
#   scripts/build_android.sh --ndk /path/to/android-ndk [options]
#
# 也可通过环境变量提供默认值：
#   ANDROID_NDK_HOME=/path/to/android-ndk scripts/build_android.sh

set -e

# 默认参数
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_STATIC="${BUILD_STATIC:-true}"
BUILD_SHARED="${BUILD_SHARED:-true}"
BUILD_TESTS="${BUILD_TESTS:-false}"
BUILD_EXAMPLES="${BUILD_EXAMPLES:-false}"
ANDROID_ABIS="${ANDROID_ABIS:-arm64-v8a,x86_64}"
ANDROID_API="${ANDROID_API:-21}"
OUTPUT_DIR="${OUTPUT_DIR:-build-android}"
NDK_PATH="${ANDROID_NDK_HOME:-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
ANDROID_CMAKE_FLAGS="${ANDROID_CMAKE_FLAGS:-}"

usage() {
    echo "Usage: $0 [--ndk /path/to/android-ndk] [--abi arm64-v8a,x86_64] [--api 21]"
    echo "          [--build-type Release|Debug] [--build-static true|false]"
    echo "          [--build-shared true|false] [--build-tests true|false]"
    echo "          [--build-examples true|false] [--output-dir build-android]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ndk)
            NDK_PATH="$2"
            shift 2
            ;;
        --abi)
            ANDROID_ABIS="$2"
            shift 2
            ;;
        --api)
            ANDROID_API="$2"
            shift 2
            ;;
        --build-type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --build-static)
            BUILD_STATIC="$2"
            shift 2
            ;;
        --build-shared)
            BUILD_SHARED="$2"
            shift 2
            ;;
        --build-tests)
            BUILD_TESTS="$2"
            shift 2
            ;;
        --build-examples)
            BUILD_EXAMPLES="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

if [[ -z "$NDK_PATH" ]]; then
    echo "Error: NDK path is required. Use --ndk or set ANDROID_NDK_HOME."
    usage
fi

TOOLCHAIN_FILE="$NDK_PATH/build/cmake/android.toolchain.cmake"
if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    echo "Error: Android toolchain file not found: $TOOLCHAIN_FILE"
    exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "Error: CMake not found"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "========================================"
echo "Kairo Android Build Script"
echo "========================================"
echo "NDK:              $NDK_PATH"
echo "ABIs:             $ANDROID_ABIS"
echo "Android API:      $ANDROID_API"
echo "Build Type:       $BUILD_TYPE"
echo "Build Static:     $BUILD_STATIC"
echo "Build Shared:     $BUILD_SHARED"
echo "Build Tests:      $BUILD_TESTS"
echo "Build Examples:   $BUILD_EXAMPLES"
echo "Output Dir:       $OUTPUT_DIR"
echo "CMake:            $(cmake --version | head -n 1)"
echo "========================================"
echo ""

IFS=',' read -ra ABI_LIST <<< "$ANDROID_ABIS"

build_variant() {
    local abi="$1"
    local shared="$2"
    local variant_dir
    local build_dir

    if [[ "$shared" == "ON" ]]; then
        variant_dir="shared"
    else
        variant_dir="static"
    fi
    build_dir="$OUTPUT_DIR/$abi/$variant_dir"

    echo "----------------------------------------"
    echo "Configuring $abi / $variant_dir"
    echo "----------------------------------------"

    # shellcheck disable=SC2086
    cmake -S "$PROJECT_ROOT" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="android-$ANDROID_API" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DKAIRO_BUILD_SHARED="$shared" \
        -DKAIRO_BUILD_TESTS="$BUILD_TESTS" \
        -DKAIRO_BUILD_EXAMPLES="$BUILD_EXAMPLES" \
        -DKAIRO_ENABLE_GPU=OFF \
        -DKAIRO_ENABLE_CUDA=OFF \
        -DKAIRO_ENABLE_OPENCL=OFF \
        -DCMAKE_INSTALL_PREFIX="$build_dir/install" \
        $ANDROID_CMAKE_FLAGS

    echo "Building $abi / $variant_dir..."
    cmake --build "$build_dir" -j "$JOBS"

    echo "Installing $abi / $variant_dir..."
    cmake --install "$build_dir"
    echo "$abi / $variant_dir completed."
    echo ""
}

for abi in "${ABI_LIST[@]}"; do
    [[ -z "$abi" ]] && continue
    if [[ "$BUILD_STATIC" == "true" ]]; then
        build_variant "$abi" OFF
    fi
    if [[ "$BUILD_SHARED" == "true" ]]; then
        build_variant "$abi" ON
    fi
done

echo "========================================"
echo "Android build completed!"
echo "========================================"
echo "Artifacts under: $OUTPUT_DIR/<abi>/<static|shared>/install"
