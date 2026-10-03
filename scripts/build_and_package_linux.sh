#!/bin/bash
# Linux 完整构建和打包脚本
# 一键构建静态库、动态库并打包成发行版本

set -e  # 遇到错误立即退出

# 默认参数
# CR-085: 版本默认值从根 CMakeLists.txt 的 project(VERSION) 提取（单一来源）；
# 环境变量 VERSION / --version 参数仍可覆盖。
_DEFAULT_VERSION="$(sed -n 's/^project(kairo[[:space:]][^)]*VERSION \([0-9][0-9.]*\).*/\1/p' \
    "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/CMakeLists.txt" | head -n1)"
VERSION="${VERSION:-${_DEFAULT_VERSION:-0.0.0}}"
unset _DEFAULT_VERSION
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_STATIC="${BUILD_STATIC:-true}"
BUILD_SHARED="${BUILD_SHARED:-true}"
BUILD_TESTS="${BUILD_TESTS:-false}"
BUILD_EXAMPLES="${BUILD_EXAMPLES:-false}"
BUILD_DIR="${BUILD_DIR:-build_linux}"
OUTPUT_DIR="${OUTPUT_DIR:-dist}"

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --version)
            VERSION="$2"
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
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--version 0.4.0] [--build-type Release|Debug] [--build-static true|false] [--build-shared true|false] [--build-tests true|false] [--build-examples true|false] [--build-dir build_linux] [--output-dir dist]"
            exit 1
            ;;
    esac
done

echo "========================================"
echo "Kairo Linux Build and Package"
echo "========================================"
echo ""

# 获取脚本目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 步骤 1: 构建
echo "Step 1/2: Building libraries..."
"$SCRIPT_DIR/build_linux.sh" \
    --build-type "$BUILD_TYPE" \
    --build-static "$BUILD_STATIC" \
    --build-shared "$BUILD_SHARED" \
    --build-tests "$BUILD_TESTS" \
    --build-examples "$BUILD_EXAMPLES" \
    --output-dir "$BUILD_DIR"

if [ $? -ne 0 ]; then
    echo "Error: Build failed"
    exit 1
fi

echo ""
echo "Step 2/2: Packaging release..."
"$SCRIPT_DIR/package_linux.sh" \
    --version "$VERSION" \
    --build-dir "$BUILD_DIR" \
    --output-dir "$OUTPUT_DIR" \
    --include-static "$BUILD_STATIC" \
    --include-shared "$BUILD_SHARED"

if [ $? -ne 0 ]; then
    echo "Error: Packaging failed"
    exit 1
fi

echo ""
echo "========================================"
echo "All done!"
echo "========================================"
