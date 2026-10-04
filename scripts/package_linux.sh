#!/bin/bash
# Linux 打包脚本
# 将构建好的库打包成发行版本

set -e  # 遇到错误立即退出

# 默认参数
# CR-085: 版本默认值从根 CMakeLists.txt 的 project(VERSION) 提取（单一来源）；
# 环境变量 VERSION / --version 参数仍可覆盖。
_DEFAULT_VERSION="$(sed -n 's/^project(kairo[[:space:]][^)]*VERSION \([0-9][0-9.]*\).*/\1/p' \
    "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/CMakeLists.txt" | head -n1)"
VERSION="${VERSION:-${_DEFAULT_VERSION:-0.0.0}}"
unset _DEFAULT_VERSION
BUILD_DIR="${BUILD_DIR:-build_linux}"
OUTPUT_DIR="${OUTPUT_DIR:-dist}"
INCLUDE_STATIC="${INCLUDE_STATIC:-true}"
INCLUDE_SHARED="${INCLUDE_SHARED:-true}"

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --version)
            VERSION="$2"
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
        --include-static)
            INCLUDE_STATIC="$2"
            shift 2
            ;;
        --include-shared)
            INCLUDE_SHARED="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--version 0.4.0] [--build-dir build_linux] [--output-dir dist] [--include-static true|false] [--include-shared true|false]"
            exit 1
            ;;
    esac
done

echo "========================================"
echo "Kairo Linux Package Script"
echo "========================================"
echo "Version: $VERSION"
echo "Build Dir: $BUILD_DIR"
echo "Output Dir: $OUTPUT_DIR"
echo "Include Static: $INCLUDE_STATIC"
echo "Include Shared: $INCLUDE_SHARED"
echo "========================================"
echo ""

# 获取项目根目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 检测架构
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)
        ARCH_NAME="x86_64"
        ;;
    aarch64|arm64)
        ARCH_NAME="aarch64"
        ;;
    *)
        ARCH_NAME="$ARCH"
        ;;
esac

# 创建输出目录
PACKAGE_NAME="kairo-${VERSION}-linux-${ARCH_NAME}"
PACKAGE_DIR="$OUTPUT_DIR/$PACKAGE_NAME"
PACKAGE_DIR_STATIC="$PACKAGE_DIR/static"
PACKAGE_DIR_SHARED="$PACKAGE_DIR/shared"

if [ -d "$PACKAGE_DIR" ]; then
    echo "Cleaning old package directory..."
    rm -rf "$PACKAGE_DIR"
fi

mkdir -p "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR_STATIC"
mkdir -p "$PACKAGE_DIR_SHARED"

echo "Starting packaging..."
echo ""

# 复制静态库
if [ "$INCLUDE_STATIC" = "true" ]; then
    STATIC_INSTALL_DIR="$BUILD_DIR/static/install"
    if [ -d "$STATIC_INSTALL_DIR" ]; then
        echo "Copying static library files..."
        cp -r "$STATIC_INSTALL_DIR"/* "$PACKAGE_DIR_STATIC/"
        
        # 验证关键文件
        LIB_FILE=$(find "$PACKAGE_DIR_STATIC" -name "libkairo.a" -o -name "kairo.a" | head -n 1)
        if [ -z "$LIB_FILE" ]; then
            echo "Warning: libkairo.a not found"
        else
            echo "  Found: $LIB_FILE"
        fi
    else
        echo "Warning: Static library install directory does not exist: $STATIC_INSTALL_DIR"
    fi
fi

# 复制动态库
if [ "$INCLUDE_SHARED" = "true" ]; then
    SHARED_INSTALL_DIR="$BUILD_DIR/shared/install"
    if [ -d "$SHARED_INSTALL_DIR" ]; then
        echo "Copying shared library files..."
        cp -r "$SHARED_INSTALL_DIR"/* "$PACKAGE_DIR_SHARED/"
        
        # 验证关键文件
        SO_FILE=$(find "$PACKAGE_DIR_SHARED" -name "libkairo.so*" | head -n 1)
        if [ -z "$SO_FILE" ]; then
            echo "Warning: libkairo.so not found"
        else
            echo "  Found: $SO_FILE"
        fi
    else
        echo "Warning: Shared library install directory does not exist: $SHARED_INSTALL_DIR"
    fi
fi

# 复制文档和许可证
echo ""
echo "Copying documentation files..."
DOCS_TO_COPY=("README.md" "LICENSE" "CHANGELOG.md")

for doc in "${DOCS_TO_COPY[@]}"; do
    SRC_PATH="$PROJECT_ROOT/$doc"
    if [ -f "$SRC_PATH" ]; then
        cp "$SRC_PATH" "$PACKAGE_DIR/"
        echo "  Copied: $doc"
    fi
done

# 创建使用指南
USAGE_GUIDE="$PACKAGE_DIR/USAGE.md"
cat > "$USAGE_GUIDE" << EOF
# Kairo Linux Distribution Package Usage Guide

## Version Information
- Version: $VERSION
- Platform: Linux
- Architecture: $ARCH_NAME

## Directory Structure

### Static Library (static/)
- \`lib/libkairo.a\` - Static library file
- \`include/kairo/\` - Header files directory
- \`lib/cmake/kairo/\` - CMake configuration files (for find_package)

### Shared Library (shared/)
- \`lib/libkairo.so\` - Shared library file (required at runtime)
- \`include/kairo/\` - Header files directory
- \`lib/cmake/kairo/\` - CMake configuration files (for find_package)

## Usage

### Using Static Library

\`\`\`cmake
find_package(kairo REQUIRED)
target_link_libraries(your_target PRIVATE kairo::kairo)
\`\`\`

Make sure to set the path when configuring CMake:
\`\`\`bash
cmake -DCMAKE_PREFIX_PATH=path/to/kairo-$VERSION-linux-$ARCH_NAME/static
\`\`\`

### Using Shared Library

\`\`\`cmake
find_package(kairo REQUIRED)
target_link_libraries(your_target PRIVATE kairo::kairo)
\`\`\`

Make sure to set the path when configuring CMake:
\`\`\`bash
cmake -DCMAKE_PREFIX_PATH=path/to/kairo-$VERSION-linux-$ARCH_NAME/shared
\`\`\`

**Note**: When using shared library, ensure \`libkairo.so\` is available at runtime:
- Copy \`libkairo.so\` to a directory in LD_LIBRARY_PATH
- Or install the library to system directories (e.g., /usr/local/lib)
- Or set LD_LIBRARY_PATH to include the directory containing \`libkairo.so\`

\`\`\`bash
export LD_LIBRARY_PATH=\$LD_LIBRARY_PATH:/path/to/kairo-$VERSION-linux-$ARCH_NAME/shared/lib
\`\`\`

## System Requirements

- Linux (kernel 3.10+)
- GCC 10+ or Clang 12+ (with C++20 support)
- CMake 3.16 or higher
- C++20 support

## More Information

Please refer to README.md and documents in docs/ directory.
EOF

echo "  Created: USAGE.md"
echo ""

# 创建压缩包
echo "Creating tar.gz package..."
# 使用绝对路径
TAR_PATH="$(cd "$OUTPUT_DIR" && pwd)/${PACKAGE_NAME}.tar.gz"

if [ -f "$TAR_PATH" ]; then
    rm -f "$TAR_PATH"
fi

# 切换到输出目录并创建压缩包
cd "$OUTPUT_DIR"
tar -czf "${PACKAGE_NAME}.tar.gz" "$PACKAGE_NAME"

echo ""
echo "========================================"
echo "Packaging completed!"
echo "========================================"
echo ""
echo "Package directory: $PACKAGE_DIR"
echo "Tar.gz package: $TAR_PATH"
echo ""

# 显示打包内容摘要
# 注意：此时工作目录已切换到 OUTPUT_DIR，所以使用相对路径
echo "Package content summary:"
if [ "$INCLUDE_STATIC" = "true" ]; then
    STATIC_LIBS=$(find "$PACKAGE_NAME/static" -name "*.a" 2>/dev/null | wc -l)
    STATIC_HEADERS=$(find "$PACKAGE_NAME/static" -name "*.hpp" 2>/dev/null | wc -l)
    echo "  Static library: $STATIC_LIBS .a files, $STATIC_HEADERS header files"
fi
if [ "$INCLUDE_SHARED" = "true" ]; then
    SHARED_SOS=$(find "$PACKAGE_NAME/shared" -name "*.so*" 2>/dev/null | wc -l)
    SHARED_HEADERS=$(find "$PACKAGE_NAME/shared" -name "*.hpp" 2>/dev/null | wc -l)
    echo "  Shared library: $SHARED_SOS .so files, $SHARED_HEADERS header files"
fi
