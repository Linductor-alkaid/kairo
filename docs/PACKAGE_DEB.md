# Ubuntu/Debian deb 打包指南

本文档说明如何在 Ubuntu/Debian 系统上将 kairo 库打包成 deb 包，方便系统级安装和管理。

---

## 快速开始

### 一键构建和打包（包含 deb）

使用提供的 bash 脚本一键完成构建、tar.gz 打包和 deb 打包：

```bash
./scripts/build_and_package_deb.sh
```

这将：
1. 构建静态库（Release 模式）
2. 构建动态库（Release 模式）
3. 打包成 tar.gz 格式
4. 打包成 deb 包（开发包和运行时包）

### 官方 CI 产物（v0.5.0 起）

推送 `v*` tag 会触发 `.github/workflows/release.yml`：amd64 deb 与 Linux tar.gz 在
`nvidia/cuda:12.4.1-devel-ubuntu22.04` 容器内完成完整构建（编译期不需要 GPU），
产物附到 GitHub Release。CUDA 执行器运行时通过动态加载 `libcuda` 降级，用户机器
没有 CUDA 驱动时不会阻止 CPU 路径使用。本地复现官方包可用同一容器执行
`scripts/build_and_package_deb.sh`。

### 在带 CUDA 的环境下（本地手工构建）

如需在本地构建与官方 CI 等价的完整 deb，建议在带 CUDA Toolkit 的构建机（或上述
devel 容器）中执行：

```bash
# 在带 CUDA Toolkit 的环境下
cmake -B build -DKAIRO_ENABLE_CUDA=ON
sudo ./scripts/build_and_package_deb.sh --version 0.6.0 --maintainer "Your Name <your.email@example.com>"
```

### 自定义构建选项

```bash
./scripts/build_and_package_deb.sh \
    --version "0.6.0" \
    --build-type "Release" \
    --maintainer "Your Name <your.email@example.com>" \
    --deb-package-type "all"
```

**参数说明：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--version` | `0.6.0` | 版本号，用于打包命名 |
| `--build-type` | `Release` | 构建类型（Release/Debug） |
| `--build-static` | `true` | 是否构建静态库 |
| `--build-shared` | `true` | 是否构建动态库 |
| `--package-deb` | `true` | 是否打包 deb 包 |
| `--deb-package-type` | `all` | deb 包类型：`all`（开发包+运行时包）、`dev`（仅开发包）、`runtime`（仅运行时包） |
| `--maintainer` | `Unknown <unknown@example.com>` | 维护者信息 |
| `--build-dir` | `build_linux` | 构建目录 |
| `--output-dir` | `dist` | 打包输出目录 |

---

## 分步操作

### 步骤 1: 构建库

首先构建库（如果还没有构建）：

```bash
./scripts/build_linux.sh --build-type Release
```

### 步骤 2: 打包 deb 包

构建完成后，使用 deb 打包脚本创建 deb 包：

```bash
# 打包所有类型（开发包 + 运行时包）
./scripts/package_deb.sh --version "0.6.0" --maintainer "Your Name <email@example.com>"

# 仅打包开发包
./scripts/package_deb.sh --version "0.6.0" --package-type dev --maintainer "Your Name <email@example.com>"

# 仅打包运行时包
./scripts/package_deb.sh --version "0.6.0" --package-type runtime --maintainer "Your Name <email@example.com>"
```

---

## deb 包说明

### 包结构

根据打包类型，脚本会生成不同的 deb 包：

#### 1. libkairo-dev（开发包）

**默认模式（`--deb-package-type all`）：**
- ✅ **推荐使用**：开发包包含所有内容（静态库、动态库、头文件、CMake 配置）
- 只需安装一个包即可使用
- 包含：
  - 静态库文件（`libkairo.a`）
  - 动态库文件（`libkairo.so*`）
  - 头文件（`/usr/include/kairo/`）
  - CMake 配置文件（`/usr/lib/cmake/kairo/`）
  - 文档文件（README.md, LICENSE, CHANGELOG.md）

**分离模式（`--deb-package-type dev`）：**
- 仅包含开发文件（静态库、头文件、CMake 配置）
- 需要同时安装 `libkairo` 运行时包（会自动安装）

#### 2. libkairo（运行时包）

包含：
- 动态库文件（`libkairo.so*`）
- 文档文件

**使用场景：**
- 仅需要运行使用 kairo 库的应用程序（不需要开发）
- 在分离模式下，开发包会自动依赖此包

### 推荐使用方式

**对于开发者（推荐）：**
```bash
# 使用默认 all 模式，只需安装一个包
sudo dpkg -i dist/libkairo-dev_0.6.0_amd64.deb
```

**对于仅运行应用程序的用户：**
```bash
# 只需安装运行时包
sudo dpkg -i dist/libkairo_0.6.0_amd64.deb
```

---

## 安装和使用

### 安装 deb 包

**推荐方式（默认 all 模式，只需安装一个包）：**
```bash
# 安装开发包（包含所有内容：静态库、动态库、头文件、CMake 配置）
sudo dpkg -i dist/libkairo-dev_0.6.0_amd64.deb

# 如果依赖缺失，修复依赖
sudo apt-get install -f

# 或者使用 apt 安装（如果已添加到仓库）
sudo apt install ./dist/libkairo-dev_0.6.0_amd64.deb
```

**说明：**
- 默认 `all` 模式下，`libkairo-dev` 包包含所有内容，**只需安装这一个包即可**
- 如果使用分离模式（`--deb-package-type dev`），安装开发包时会自动安装运行时包作为依赖
- 如果只需要运行应用程序（不需要开发），可以只安装 `libkairo` 运行时包

### 验证安装

```bash
# 检查库文件
ls /usr/lib/libkairo*
ls /usr/include/kairo/

# 检查 CMake 配置
ls /usr/lib/cmake/kairo/

# 查看包信息
dpkg -L libkairo-dev
dpkg -L libkairo
```

### 在其他项目中使用

安装 deb 包后，可以直接使用 CMake 的 `find_package`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_project)

find_package(kairo REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE kairo::kairo)
```

编译时不需要指定路径：

```bash
cmake -B build
cmake --build build
```

---

## 卸载

```bash
# 卸载开发包
sudo apt remove libkairo-dev

# 卸载运行时包（如果不再需要）
sudo apt remove libkairo
```

---

## 系统要求

- **操作系统**: Ubuntu 18.04+ 或 Debian 10+
- **工具**: `dpkg-dev` 包（包含 `dpkg-deb` 工具）
- **编译器**: GCC 10+ 或 Clang 12+（支持 C++20）
- **CMake**: 3.16 或更高版本

### 安装依赖工具

如果系统没有 `dpkg-deb`，需要安装：

```bash
sudo apt-get update
sudo apt-get install dpkg-dev
```

---

## 常见问题

### Q: 如何选择包类型？

A: 
- **all**（推荐）：生成包含所有内容的开发包（静态库+动态库+头文件+CMake配置），**只需安装一个包即可使用**。同时也会生成独立的运行时包（可选）。
- **dev**：仅生成开发包（静态库+头文件+CMake配置），需要同时安装运行时包（会自动作为依赖安装）
- **runtime**：仅生成运行时包（动态库），用于仅需要运行应用程序的场景

**对于大多数用户，推荐使用 `all` 模式，只需安装 `libkairo-dev` 一个包即可。**

### Q: 打包失败，提示找不到 dpkg-deb？

A: 安装 `dpkg-dev` 包：

```bash
sudo apt-get install dpkg-dev
```

### Q: 如何修改包的维护者信息？

A: 使用 `--maintainer` 参数：

```bash
./scripts/package_deb.sh --maintainer "Your Name <your.email@example.com>"
```

### Q: deb 包安装后找不到库？

A: 检查库文件是否正确安装：

```bash
# 检查库文件
ls -la /usr/lib/libkairo*

# 检查动态库链接
ldconfig
ldconfig -p | grep kairo
```

### Q: 如何创建适用于不同架构的 deb 包？

A: 脚本会自动检测当前系统架构。如果需要交叉编译，需要：
1. 在目标架构的系统上构建
2. 或使用交叉编译工具链，然后手动调整架构标识

### Q: 如何查看 deb 包内容？

A: 使用 `dpkg-deb` 命令：

```bash
# 查看包信息
dpkg-deb -I dist/libkairo-dev_0.6.0_amd64.deb

# 查看包内容
dpkg-deb -c dist/libkairo-dev_0.6.0_amd64.deb

# 提取包内容（不解压）
dpkg-deb -x dist/libkairo-dev_0.6.0_amd64.deb /tmp/extracted
```

### Q: 如何创建本地 apt 仓库？

A: 可以使用 `reprepro` 或 `aptly` 工具创建本地仓库。基本步骤：

```bash
# 安装 reprepro
sudo apt-get install reprepro

# 创建仓库目录结构
mkdir -p repo/conf

# 创建 distributions 文件
cat > repo/conf/distributions << EOF
Codename: focal
Components: main
Architectures: amd64
EOF

# 添加 deb 包
reprepro -b repo includedeb focal dist/*.deb
```

---

## 发布检查清单

在发布 deb 包前，请确认：

- [ ] 版本号正确：`CMakeLists.txt`、README 与打包命令均为 `0.6.0`
- [ ] 维护者信息正确
- [ ] 静态库和动态库都已成功构建
- [ ] v0.5.0 发布包在带 CUDA Toolkit 的环境下完成完整构建（`KAIRO_ENABLE_CUDA=ON`）
- [ ] 无 CUDA 用户机器验证运行时降级路径：CPU 功能可用，CUDA 后端不可用时不崩溃
- [ ] 所有头文件都已包含在开发包中
- [ ] CMake 配置文件已正确生成
- [ ] 文档文件（README.md, README_zh.md, LICENSE, CHANGELOG.md, docs/API.md, docs/MIGRATION.md）已包含
- [ ] deb 包已创建
- [ ] 在测试环境中验证了 deb 包的安装和使用
- [ ] 验证了包的依赖关系正确

---

## 相关文档

- [BUILD.md](BUILD.md) - 通用构建说明
- [PACKAGE_LINUX.md](PACKAGE_LINUX.md) - Linux tar.gz 打包指南
- [PACKAGE_WINDOWS.md](PACKAGE_WINDOWS.md) - Windows 打包指南
- [API.md](API.md) - API 使用文档
- [README.md](../README.md) - 项目说明

---

## 高级用法

### 在无 CUDA 环境下

如果用户机器没有 CUDA Toolkit，可以关闭 CUDA 构建；kairo 的 CPU 功能和 OpenCL 可选路径不受影响。使用 v0.5.0 CUDA 完整包时，CUDA 后端也会在运行时检测不可用并自动降级。

```bash
# 用户机器没有 CUDA Toolkit: 运行时自动降级
cmake -B build -DKAIRO_ENABLE_CUDA=OFF
```

### 自定义包描述

```bash
./scripts/package_deb.sh \
    --version "0.6.0" \
    --maintainer "Your Name <email@example.com>" \
    --description "Custom description for the package"
```

### 仅打包已构建的库

如果已经构建完成，可以直接打包：

```bash
./scripts/package_deb.sh \
    --version "0.6.0" \
    --build-dir build_linux \
    --maintainer "Your Name <email@example.com>"
```

### 分离开发和运行时包

```bash
# 先打包开发包
./scripts/package_deb.sh --package-type dev --version "0.6.0"

# 再打包运行时包
./scripts/package_deb.sh --package-type runtime --version "0.6.0"
```
