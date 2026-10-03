#!/bin/bash
# run_coverage.sh - 一键生成测试代码覆盖率报告（gcov/lcov）
# 用法：在项目根目录执行 ./scripts/run_coverage.sh

set -e
cd "$(dirname "$0")/.."
BUILD_DIR="${BUILD_DIR:-build}"

echo "=========================================="
echo "Kairo 代码覆盖率"
echo "=========================================="
echo ""

# 检查 lcov
if ! command -v lcov &>/dev/null || ! command -v genhtml &>/dev/null; then
    echo "错误: 未找到 lcov 或 genhtml。请先安装："
    echo "  Ubuntu/Debian: sudo apt-get install lcov"
    echo "  RHEL/CentOS:   sudo yum install lcov"
    echo "  macOS:         brew install lcov"
    exit 1
fi

# 1. 配置（启用覆盖率，Debug 构建）
echo "[1/5] 配置 CMake（启用覆盖率）..."
cmake -B "$BUILD_DIR" -S . \
  -DKAIRO_ENABLE_COVERAGE=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DKAIRO_BUILD_EXAMPLES=OFF \
  -DKAIRO_BUILD_TESTS=ON

# 2. 编译
echo ""
echo "[2/5] 编译..."
cmake --build "$BUILD_DIR"

# 3. 运行测试（unit + integration，不含 benchmark/stress 以节省时间）
echo ""
echo "[3/5] 运行测试（unit + integration）..."
ctest --test-dir "$BUILD_DIR" -L "unit|integration" --output-on-failure

# 4. 收集覆盖率
echo ""
echo "[4/5] 收集覆盖率（lcov）..."
cd "$BUILD_DIR"
lcov --capture --directory . --output-file coverage.info

# 5. 过滤并生成 HTML
echo ""
echo "[5/5] 过滤无关路径并生成 HTML 报告..."
lcov --remove coverage.info \
  '*/tests/*' \
  '*/test_*' \
  '*/examples/*' \
  '*/usr/*' \
  '*/opt/*' \
  '*/include/c++/*' \
  '*/include/gtest/*' \
  '*/include/gmock/*' \
  --output-file coverage_filtered.info

genhtml coverage_filtered.info \
  --output-directory coverage_html \
  --title "Kairo Code Coverage" \
  --show-details \
  --legend

echo ""
echo "=========================================="
echo "覆盖率报告已生成"
echo "=========================================="
echo ""
echo "  HTML 报告: $BUILD_DIR/coverage_html/index.html"
echo ""
lcov --summary coverage_filtered.info 2>/dev/null || true
echo ""
echo "在浏览器中打开上述 HTML 文件可查看逐行覆盖率。"
echo ""
