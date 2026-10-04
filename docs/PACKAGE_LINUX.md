# Linux 打包指南

本文档说明如何在 Linux 平台上将 kairo 库打包成静态库和动态库，用于发行。

---

## 快速开始

### 一键构建和打包

使用提供的 bash 脚本一键完成构建和打包：

```bash
./scripts/build_and_package_linux.sh
```

这将：
1. 构建静态库（Release 模式）
2. 构建动态库（Release 模式）
3. 打包成发行版本（tar.gz 格式）

### 自定义构建选项

```bash
./scripts/build_and_package_linux.sh \
    --version "0.6.0" \
    --build-type "Release" \
    --build-static true \
    --build-shared true
```

**参数说明：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--version` | `0.6.0` | 版本号，用于打包命名 |
| `--build-type` | `Release` | 构建类型（Release/Debug） |
| `--build-static` | `true` | 是否构建静态库 |
| `--build-shared` | `true` | 是否构建动态库 |
| `--build-tests` | `false` | 是否构建测试 |
| `--build-examples` | `false` | 是否构建示例 |
| `--build-dir` | `build_linux` | 构建目录 |
| `--output-dir` | `dist` | 打包输出目录 |

---

## 分步操作

### 步骤 1: 构建库

使用构建脚本分别构建静态库和动态库：

```bash
# 构建静态库和动态库
./scripts/build_linux.sh --build-type Release

# 仅构建静态库
./scripts/build_linux.sh --build-type Release --build-static true --build-shared false

# 仅构建动态库
./scripts/build_linux.sh --build-type Release --build-static false --build-shared true
```

### 步骤 2: 打包发行版本

构建完成后，使用打包脚本创建发行包：

```bash
./scripts/package_linux.sh --version "0.6.0"
```

打包脚本会：
- 复制静态库和动态库的安装文件
- 复制文档（README.md, LICENSE, CHANGELOG.md）
- 创建使用说明（USAGE.md）
- 生成 tar.gz 压缩包

---

## 手动构建（不使用脚本）

### 构建静态库

```bash
# 配置
cmake -B build_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DKAIRO_BUILD_SHARED=OFF \
    -DKAIRO_BUILD_TESTS=OFF \
    -DKAIRO_BUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX=build_static/install

# 构建
cmake --build build_static -j$(nproc)

# 安装
cmake --install build_static
```

### 构建动态库

```bash
# 配置
cmake -B build_shared \
    -DCMAKE_BUILD_TYPE=Release \
    -DKAIRO_BUILD_SHARED=ON \
    -DKAIRO_BUILD_TESTS=OFF \
    -DKAIRO_BUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX=build_shared/install

# 构建
cmake --build build_shared -j$(nproc)

# 安装
cmake --install build_shared
```

---

## 打包目录结构

打包后的目录结构如下：

```
kairo-0.6.0-linux-x86_64/
├── static/                    # 静态库
│   ├── lib/
│   │   ├── libkairo.a      # 静态库文件
│   │   └── cmake/
│   │       └── kairo/      # CMake 配置文件
│   └── include/
│       └── kairo/          # 头文件
├── shared/                    # 动态库
│   ├── lib/
│   │   ├── libkairo.so     # 动态库文件（运行时）
│   │   └── cmake/
│   │       └── kairo/      # CMake 配置文件
│   └── include/
│       └── kairo/          # 头文件
├── README.md
├── LICENSE
├── CHANGELOG.md
└── USAGE.md                   # 使用说明
```

---

## 在其他项目中使用

### 使用静态库

1. 解压发行包
2. 在 CMake 配置时设置路径：

```bash
cmake -B build -DCMAKE_PREFIX_PATH=path/to/kairo-0.6.0-linux-x86_64/static
```

3. 在项目的 `CMakeLists.txt` 中：

```cmake
find_package(kairo REQUIRED)
target_link_libraries(your_target PRIVATE kairo::kairo)
```

### 使用动态库

1. 解压发行包
2. 在 CMake 配置时设置路径：

```bash
cmake -B build -DCMAKE_PREFIX_PATH=path/to/kairo-0.6.0-linux-x86_64/shared
```

3. 在项目的 `CMakeLists.txt` 中：

```cmake
find_package(kairo REQUIRED)
target_link_libraries(your_target PRIVATE kairo::kairo)
```

4. **重要**: 确保 `libkairo.so` 在运行时可用：
   - 将 `libkairo.so` 复制到系统库目录（如 `/usr/local/lib`）
   - 或将包含 `libkairo.so` 的目录添加到 `LD_LIBRARY_PATH` 环境变量
   - 或使用 `rpath` 在链接时指定库路径

```bash
# 方法 1: 设置 LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/kairo-0.6.0-linux-x86_64/shared/lib

# 方法 2: 安装到系统目录
sudo cp /path/to/kairo-0.6.0-linux-x86_64/shared/lib/libkairo.so* /usr/local/lib/
sudo ldconfig
```

---

## 系统要求

- **操作系统**: Linux (kernel 3.10+)
- **编译器**: GCC 10+ 或 Clang 12+（支持 C++20）
- **CMake**: 3.16 或更高版本
- **C++ 标准**: C++20

---

## 常见问题

### Q: 如何选择编译器？

A: 使用 `CC` 和 `CXX` 环境变量：

```bash
# 使用 GCC
CC=gcc CXX=g++ ./scripts/build_linux.sh

# 使用 Clang
CC=clang CXX=clang++ ./scripts/build_linux.sh
```

### Q: 如何构建不同架构的版本？

A: 脚本会自动检测当前系统架构。如果需要交叉编译，需要设置相应的 CMake 工具链文件：

```bash
cmake -B build_static \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DKAIRO_BUILD_SHARED=OFF
```

### Q: 构建失败，提示找不到 CMake？

A: 确保 CMake 已安装并在 PATH 环境变量中。可以运行 `cmake --version` 验证。

### Q: 使用动态库时提示找不到 libkairo.so？

A: 确保 `libkairo.so` 在以下位置之一：
- 系统库目录（如 `/usr/local/lib`，需要运行 `ldconfig`）
- `LD_LIBRARY_PATH` 环境变量中的目录
- 可执行文件所在目录（如果使用相对路径）

可以使用 `ldd` 命令检查可执行文件的库依赖：

```bash
ldd your_executable | grep kairo
```

### Q: 如何同时构建 Debug 和 Release 版本？

A: 分别运行两次构建：

```bash
# Debug 版本
./scripts/build_linux.sh --build-type Debug --output-dir build_linux_debug

# Release 版本
./scripts/build_linux.sh --build-type Release --output-dir build_linux_release
```

### Q: 如何查看打包内容？

A: 解压 tar.gz 文件查看：

```bash
tar -tzf dist/kairo-0.6.0-linux-x86_64.tar.gz
```

或解压到临时目录：

```bash
mkdir -p /tmp/package_test
tar -xzf dist/kairo-0.6.0-linux-x86_64.tar.gz -C /tmp/package_test
tree /tmp/package_test
```

---

## 验证构建结果

构建完成后，可以验证关键文件：

### 静态库
- `build_linux/static/install/lib/libkairo.a` - 静态库文件
- `build_linux/static/install/include/kairo/` - 头文件目录

### 动态库
- `build_linux/shared/install/lib/libkairo.so` - 动态库文件
- `build_linux/shared/install/include/kairo/` - 头文件目录

可以使用以下命令验证库文件：

```bash
# 查看静态库内容
ar -t build_linux/static/install/lib/libkairo.a

# 查看动态库信息
ldd build_linux/shared/install/lib/libkairo.so
readelf -d build_linux/shared/install/lib/libkairo.so
```

---

## 发布检查清单

在发布前，请确认：

- [ ] 版本号正确（在 `CMakeLists.txt` 和打包脚本中）
- [ ] 静态库和动态库都已成功构建
- [ ] 所有头文件都已包含在打包中
- [ ] CMake 配置文件已正确生成
- [ ] 文档文件（README.md, LICENSE, CHANGELOG.md）已包含
- [ ] 使用说明（USAGE.md）已生成
- [ ] tar.gz 压缩包已创建
- [ ] 在测试环境中验证了静态库和动态库的使用
- [ ] 验证了不同 Linux 发行版的兼容性（如 Ubuntu, CentOS, Debian 等）

---

## 相关文档

- [BUILD.md](BUILD.md) - 通用构建说明
- [API.md](API.md) - API 使用文档
- [README.md](../README.md) - 项目说明
- [PACKAGE_WINDOWS.md](PACKAGE_WINDOWS.md) - Windows 打包指南
