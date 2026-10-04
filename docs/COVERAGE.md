# 测试代码覆盖率说明

本项目使用 **gcov**（GCC/Clang 内置）和 **lcov**（生成 HTML 报告）获取测试的代码覆盖率。

---

## 前置条件

1. **编译器**：GCC 或 Clang（MSVC 暂不支持）。
2. **lcov**：用于收集覆盖率数据并生成 HTML 报告。
   - Ubuntu/Debian: `sudo apt-get install lcov`
   - RHEL/CentOS: `sudo yum install lcov`
   - macOS: `brew install lcov`

---

## 获取覆盖率的步骤

### 1. 使用覆盖率选项配置并构建

```bash
# 启用覆盖率，建议使用 Debug 构建
cmake -B build -S . \
  -DKAIRO_ENABLE_COVERAGE=ON \
  -DCMAKE_BUILD_TYPE=Debug

# 编译（会为 kairo 库和测试加上 --coverage）
cmake --build build
```

### 2. 运行测试

测试运行时会生成 `.gcda` 文件（在 build 目录及子目录中），与编译时生成的 `.gcno` 一起供 gcov/lcov 使用。

```bash
# 运行单元测试和集成测试（推荐，耗时较短）
ctest --test-dir build -L "unit|integration" --output-on-failure

# 或运行全部测试（含性能/压力测试，耗时更长）
ctest --test-dir build --output-on-failure
```

### 3. 用 lcov 收集覆盖率并生成报告

```bash
cd build

# 收集覆盖率原始数据
lcov --capture --directory . --output-file coverage.info

# 排除测试代码、示例、系统头文件等（只看库代码覆盖率）
lcov --remove coverage.info \
  '*/tests/*' \
  '*/test_*' \
  '*/examples/*' \
  '*/usr/*' \
  '*/opt/*' \
  '*/include/c++/*' \
  --output-file coverage_filtered.info

# 生成 HTML 报告
genhtml coverage_filtered.info --output-directory coverage_html \
  --title "Kairo Code Coverage" --show-details --legend
```

### 4. 查看覆盖率

- **HTML 报告**：在浏览器中打开 `build/coverage_html/index.html`。
- **终端摘要**：`lcov --summary coverage_filtered.info` 可打印行覆盖率、函数覆盖率等简要信息。

```bash
lcov --summary coverage_filtered.info
```

---

## 一键脚本（可选）

项目根目录下的 `scripts/run_coverage.sh` 可按顺序执行：配置（启用覆盖率）→ 编译 → 运行测试 → lcov 收集 → genhtml 生成 HTML。  
确保已安装 lcov，然后在项目根目录执行：

```bash
./scripts/run_coverage.sh
```

完成后在 `build/coverage_html/index.html` 查看报告。

---

## 常见问题

| 问题 | 处理 |
|------|------|
| `lcov` / `genhtml` 未找到 | 安装 lcov：`sudo apt-get install lcov`（或对应系统命令）。 |
| 覆盖率始终为 0 | 确认配置时使用了 `-DKAIRO_ENABLE_COVERAGE=ON` 且已重新 `cmake --build build`；运行过 `ctest`。 |
| 报告中包含大量 `/usr/*` 等无关文件 | 使用 `lcov --remove` 排除后再 `genhtml`，参见上述命令。 |
| Windows / MSVC | 当前仅支持 GCC/Clang；Windows 下可考虑 WSL 或 MinGW，或后续接入 OpenCppCoverage 等方案。 |

---

## 参考

- [gcov](https://gcc.gnu.org/onlinedocs/gcc/Gcov.html)
- [lcov](http://ltp.sourceforge.net/coverage/lcov.php)
