#!/bin/bash
# Ubuntu/Debian deb 打包脚本
# 将构建好的库打包成 deb 包

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
PACKAGE_TYPE="${PACKAGE_TYPE:-all}"  # all, dev, runtime
MAINTAINER="${MAINTAINER:-Unknown <unknown@example.com>}"
DESCRIPTION="${DESCRIPTION:-C++ kairo library for task scheduling and thread pool management}"

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
        --package-type)
            PACKAGE_TYPE="$2"
            shift 2
            ;;
        --maintainer)
            MAINTAINER="$2"
            shift 2
            ;;
        --description)
            DESCRIPTION="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--version 0.4.0] [--build-dir build_linux] [--output-dir dist] [--package-type all|dev|runtime] [--maintainer \"Name <email>\"] [--description \"Description\"]"
            exit 1
            ;;
    esac
done

echo "========================================"
echo "Kairo Debian Package Script"
echo "========================================"
echo "Version: $VERSION"
echo "Build Dir: $BUILD_DIR"
echo "Output Dir: $OUTPUT_DIR"
echo "Package Type: $PACKAGE_TYPE"
echo "Maintainer: $MAINTAINER"
echo "========================================"
echo ""

# 检查 dpkg-deb
if ! command -v dpkg-deb &> /dev/null; then
    echo "Error: dpkg-deb not found. Please install dpkg-dev package:"
    echo "  sudo apt-get install dpkg-dev"
    exit 1
fi

# 获取项目根目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 检测架构
ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)
case "$ARCH" in
    amd64)
        DEB_ARCH="amd64"
        ;;
    arm64)
        DEB_ARCH="arm64"
        ;;
    *)
        # 尝试从 uname -m 转换
        case "$(uname -m)" in
            x86_64)
                DEB_ARCH="amd64"
                ;;
            aarch64)
                DEB_ARCH="arm64"
                ;;
            *)
                DEB_ARCH="$ARCH"
                ;;
        esac
        ;;
esac

# 创建临时打包目录
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

# deb 版本号（去掉可能的 -dev 后缀）
DEB_VERSION=$(echo "$VERSION" | sed 's/-.*$//')
PACKAGE_BASE="libkairo"

# 函数：创建开发包
create_dev_package() {
    echo "Creating development package (libkairo-dev)..."
    
    PACKAGE_NAME="${PACKAGE_BASE}-dev"
    DEB_DIR="$TEMP_DIR/${PACKAGE_NAME}_${DEB_VERSION}_${DEB_ARCH}"
    
    # 创建目录结构
    mkdir -p "$DEB_DIR/DEBIAN"
    mkdir -p "$DEB_DIR/usr/lib"
    mkdir -p "$DEB_DIR/usr/include"
    mkdir -p "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}"
    
    # 复制静态库
    if [ -d "$BUILD_DIR/static/install" ]; then
        echo "  Copying static library..."
        # 复制库文件（.a 文件）
        if [ -d "$BUILD_DIR/static/install/lib" ]; then
            find "$BUILD_DIR/static/install/lib" -maxdepth 1 -name "*.a" -exec cp {} "$DEB_DIR/usr/lib/" \;
        fi
        # 复制头文件
        if [ -d "$BUILD_DIR/static/install/include" ]; then
            cp -r "$BUILD_DIR/static/install/include"/* "$DEB_DIR/usr/include/" 2>/dev/null || true
        fi
    fi
    
    # 如果使用 all-in-one 模式，也复制动态库到开发包
    if [ "$PACKAGE_TYPE" = "all" ] && [ -d "$BUILD_DIR/shared/install" ]; then
        echo "  Copying shared library (all-in-one package)..."
        if [ -d "$BUILD_DIR/shared/install/lib" ]; then
            find "$BUILD_DIR/shared/install/lib" -name "*.so*" -exec cp -a {} "$DEB_DIR/usr/lib/" \;
        fi
    fi
    
    # 复制 CMake 配置文件（如果存在）
    if [ -d "$BUILD_DIR/static/install/lib/cmake" ]; then
        mkdir -p "$DEB_DIR/usr/lib/cmake"
        cp -r "$BUILD_DIR/static/install/lib/cmake"/* "$DEB_DIR/usr/lib/cmake/" 2>/dev/null || true
    fi
    
    # 复制文档
    if [ -f "$PROJECT_ROOT/README.md" ]; then
        cp "$PROJECT_ROOT/README.md" "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}/"
    fi
    if [ -f "$PROJECT_ROOT/LICENSE" ]; then
        cp "$PROJECT_ROOT/LICENSE" "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}/copyright"
    fi
    if [ -f "$PROJECT_ROOT/CHANGELOG.md" ]; then
        cp "$PROJECT_ROOT/CHANGELOG.md" "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}/changelog"
        gzip -9 "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}/changelog" 2>/dev/null || true
    fi
    
    # 创建 control 文件
    # 如果使用 all-in-one 模式，开发包不依赖运行时包（因为已经包含了）
    if [ "$PACKAGE_TYPE" = "all" ]; then
        DEPS="libc6 (>= 2.17), libstdc++6 (>= 5.2)"
        DESC_EXTRA=" This package contains all files needed to develop and run applications using the kairo library (header files, static library, shared library, and CMake configuration files)."
    else
        DEPS="libc6 (>= 2.17), libstdc++6 (>= 5.2), ${PACKAGE_BASE} (= ${DEB_VERSION})"
        DESC_EXTRA=" This package contains header files, static library, and CMake configuration files needed to develop applications using the kairo library. The runtime library (${PACKAGE_BASE}) will be automatically installed as a dependency."
    fi
    
    cat > "$DEB_DIR/DEBIAN/control" << EOF
Package: ${PACKAGE_NAME}
Version: ${DEB_VERSION}
Section: libdevel
Priority: optional
Architecture: ${DEB_ARCH}
Depends: ${DEPS}
Maintainer: ${MAINTAINER}
Description: ${DESCRIPTION} (development files)${DESC_EXTRA}
EOF
    
    # 创建 copyright 文件
    if [ -f "$PROJECT_ROOT/LICENSE" ]; then
        cat > "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}/copyright" << EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: kairo
Source: <url>

Files: *
Copyright: $(date +%Y) ${MAINTAINER%% *}
License: $(head -n 1 "$PROJECT_ROOT/LICENSE" 2>/dev/null || echo "See LICENSE file")

$(cat "$PROJECT_ROOT/LICENSE" 2>/dev/null || echo "See LICENSE file for details")
EOF
    fi
    
    # 创建 postinst 脚本（可选）
    # 创建 prerm 脚本（可选）
    
    # 设置文件权限
    find "$DEB_DIR" -type f -exec chmod 644 {} \;
    find "$DEB_DIR" -type d -exec chmod 755 {} \;
    chmod 755 "$DEB_DIR/DEBIAN" 2>/dev/null || true
    
    # 构建 deb 包
    echo "  Building deb package..."
    dpkg-deb --build "$DEB_DIR" "$OUTPUT_DIR/${PACKAGE_NAME}_${DEB_VERSION}_${DEB_ARCH}.deb"
    
    echo "  Created: $OUTPUT_DIR/${PACKAGE_NAME}_${DEB_VERSION}_${DEB_ARCH}.deb"
}

# 函数：创建运行时包
create_runtime_package() {
    echo "Creating runtime package (libkairo)..."
    
    PACKAGE_NAME="${PACKAGE_BASE}"
    DEB_DIR="$TEMP_DIR/${PACKAGE_NAME}_${DEB_VERSION}_${DEB_ARCH}"
    
    # 创建目录结构
    mkdir -p "$DEB_DIR/DEBIAN"
    mkdir -p "$DEB_DIR/usr/lib"
    mkdir -p "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}"
    
    # 复制动态库
    if [ -d "$BUILD_DIR/shared/install" ]; then
        echo "  Copying shared library..."
        # 复制所有 .so 文件（包括符号链接）
        if [ -d "$BUILD_DIR/shared/install/lib" ]; then
            find "$BUILD_DIR/shared/install/lib" -name "*.so*" -exec cp -a {} "$DEB_DIR/usr/lib/" \;
        fi
    fi
    
    # 复制文档
    if [ -f "$PROJECT_ROOT/README.md" ]; then
        mkdir -p "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}"
        cp "$PROJECT_ROOT/README.md" "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}/"
    fi
    if [ -f "$PROJECT_ROOT/LICENSE" ]; then
        mkdir -p "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}"
        cp "$PROJECT_ROOT/LICENSE" "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}/copyright"
    fi
    
    # 创建 control 文件
    cat > "$DEB_DIR/DEBIAN/control" << EOF
Package: ${PACKAGE_NAME}
Version: ${DEB_VERSION}
Section: libs
Priority: optional
Architecture: ${DEB_ARCH}
Depends: libc6 (>= 2.17), libstdc++6 (>= 5.2)
Maintainer: ${MAINTAINER}
Description: ${DESCRIPTION} (runtime library)
 This package contains the runtime library files needed to run applications
 that use the kairo library.
EOF
    
    # 创建 copyright 文件
    if [ -f "$PROJECT_ROOT/LICENSE" ]; then
        mkdir -p "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}"
        cat > "$DEB_DIR/usr/share/doc/${PACKAGE_NAME}/copyright" << EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: kairo
Source: <url>

Files: *
Copyright: $(date +%Y) ${MAINTAINER%% *}
License: $(head -n 1 "$PROJECT_ROOT/LICENSE" 2>/dev/null || echo "See LICENSE file")

$(cat "$PROJECT_ROOT/LICENSE" 2>/dev/null || echo "See LICENSE file for details")
EOF
    fi
    
    # 设置文件权限
    find "$DEB_DIR" -type f -exec chmod 644 {} \;
    find "$DEB_DIR" -type d -exec chmod 755 {} \;
    chmod 755 "$DEB_DIR/DEBIAN" 2>/dev/null || true
    
    # 构建 deb 包
    echo "  Building deb package..."
    dpkg-deb --build "$DEB_DIR" "$OUTPUT_DIR/${PACKAGE_NAME}_${DEB_VERSION}_${DEB_ARCH}.deb"
    
    echo "  Created: $OUTPUT_DIR/${PACKAGE_NAME}_${DEB_VERSION}_${DEB_ARCH}.deb"
}

# 创建输出目录
mkdir -p "$OUTPUT_DIR"

# 根据包类型创建相应的包
case "$PACKAGE_TYPE" in
    dev)
        create_dev_package
        ;;
    runtime)
        create_runtime_package
        ;;
    all)
        # all 模式：创建包含所有内容的开发包（推荐，只需安装一个包）
        echo "Creating all-in-one development package (includes runtime library)..."
        create_dev_package
        echo ""
        # 仍然创建独立的运行时包（可选，用于仅需要运行时的场景）
        echo "Creating separate runtime package (optional)..."
        create_runtime_package
        ;;
    *)
        echo "Error: Invalid package type: $PACKAGE_TYPE"
        echo "Valid types: all, dev, runtime"
        exit 1
        ;;
esac

echo ""
echo "========================================"
echo "Debian packaging completed!"
echo "========================================"
echo ""
echo "Package(s) created in: $OUTPUT_DIR"
echo ""

# 显示包信息
if [ "$PACKAGE_TYPE" = "all" ] || [ "$PACKAGE_TYPE" = "dev" ]; then
    if [ -f "$OUTPUT_DIR/${PACKAGE_BASE}-dev_${DEB_VERSION}_${DEB_ARCH}.deb" ]; then
        echo "Development package:"
        dpkg-deb -I "$OUTPUT_DIR/${PACKAGE_BASE}-dev_${DEB_VERSION}_${DEB_ARCH}.deb" | head -10
        echo ""
    fi
fi

if [ "$PACKAGE_TYPE" = "all" ] || [ "$PACKAGE_TYPE" = "runtime" ]; then
    if [ -f "$OUTPUT_DIR/${PACKAGE_BASE}_${DEB_VERSION}_${DEB_ARCH}.deb" ]; then
        echo "Runtime package:"
        dpkg-deb -I "$OUTPUT_DIR/${PACKAGE_BASE}_${DEB_VERSION}_${DEB_ARCH}.deb" | head -10
        echo ""
    fi
fi
