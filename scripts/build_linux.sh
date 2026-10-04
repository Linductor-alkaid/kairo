#!/bin/bash
# Linux 构建脚本
# 用于构建 kairo 库的静态库和动态库

set -e  # 遇到错误立即退出

# 默认参数
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_STATIC="${BUILD_STATIC:-true}"
BUILD_SHARED="${BUILD_SHARED:-true}"
BUILD_TESTS="${BUILD_TESTS:-false}"
BUILD_EXAMPLES="${BUILD_EXAMPLES:-false}"
OUTPUT_DIR="${OUTPUT_DIR:-build_linux}"

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
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
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--build-type Release|Debug] [--build-static true|false] [--build-shared true|false] [--build-tests true|false] [--build-examples true|false] [--output-dir build_linux]"
            exit 1
            ;;
    esac
done

echo "========================================"
echo "Kairo Linux Build Script"
echo "========================================"
echo "Build Type: $BUILD_TYPE"
echo "Build Static: $BUILD_STATIC"
echo "Build Shared: $BUILD_SHARED"
echo "Build Tests: $BUILD_TESTS"
echo "Build Examples: $BUILD_EXAMPLES"
echo "Output Dir: $OUTPUT_DIR"
echo "========================================"
echo ""

# 检查 CMake
if ! command -v cmake &> /dev/null; then
    echo "Error: CMake not found. Please ensure CMake is installed and in PATH"
    exit 1
fi

echo "Found CMake: $(which cmake)"
echo "CMake version: $(cmake --version | head -n 1)"
echo ""

# 获取项目根目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 构建静态库
if [ "$BUILD_STATIC" = "true" ]; then
    echo "========================================"
    echo "Building Static Library"
    echo "========================================"
    
    STATIC_BUILD_DIR="$OUTPUT_DIR/static"
    
    # 配置
    echo "Configuring static library build..."
    cmake -B "$STATIC_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DKAIRO_BUILD_SHARED=OFF \
        -DKAIRO_BUILD_TESTS="$BUILD_TESTS" \
        -DKAIRO_BUILD_EXAMPLES="$BUILD_EXAMPLES" \
        -DCMAKE_INSTALL_PREFIX="$STATIC_BUILD_DIR/install"
    
    if [ $? -ne 0 ]; then
        echo "Error: CMake configuration failed"
        exit 1
    fi
    
    # 构建
    echo "Building static library..."
    cmake --build "$STATIC_BUILD_DIR" -j$(nproc)
    
    if [ $? -ne 0 ]; then
        echo "Error: Build failed"
        exit 1
    fi
    
    # 安装
    echo "Installing static library..."
    cmake --install "$STATIC_BUILD_DIR"
    
    if [ $? -ne 0 ]; then
        echo "Error: Installation failed"
        exit 1
    fi
    
    echo "Static library build completed!"
    echo ""
fi

# 构建动态库
if [ "$BUILD_SHARED" = "true" ]; then
    echo "========================================"
    echo "Building Shared Library"
    echo "========================================"
    
    SHARED_BUILD_DIR="$OUTPUT_DIR/shared"
    
    # 配置
    echo "Configuring shared library build..."
    cmake -B "$SHARED_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DKAIRO_BUILD_SHARED=ON \
        -DKAIRO_BUILD_TESTS="$BUILD_TESTS" \
        -DKAIRO_BUILD_EXAMPLES="$BUILD_EXAMPLES" \
        -DCMAKE_INSTALL_PREFIX="$SHARED_BUILD_DIR/install"
    
    if [ $? -ne 0 ]; then
        echo "Error: CMake configuration failed"
        exit 1
    fi
    
    # 构建
    echo "Building shared library..."
    cmake --build "$SHARED_BUILD_DIR" -j$(nproc)
    
    if [ $? -ne 0 ]; then
        echo "Error: Build failed"
        exit 1
    fi
    
    # 安装
    echo "Installing shared library..."
    cmake --install "$SHARED_BUILD_DIR"
    
    if [ $? -ne 0 ]; then
        echo "Error: Installation failed"
        exit 1
    fi
    
    echo "Shared library build completed!"
    echo ""
fi

echo "========================================"
echo "Build completed!"
echo "========================================"
echo ""
echo "Build artifacts location:"
if [ "$BUILD_STATIC" = "true" ]; then
    echo "  Static library: $OUTPUT_DIR/static/install"
fi
if [ "$BUILD_SHARED" = "true" ]; then
    echo "  Shared library: $OUTPUT_DIR/shared/install"
fi
