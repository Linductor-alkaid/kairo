# Windows 打包指南

本文档说明如何在 Windows 平台上将 kairo 库打包成静态库和动态库，用于发行。

> **Windows (MSVC) 限制**：动态库当前仅支持 GCC/Clang（Linux/Android）。库尚未
> 声明 `dllexport` 注解，MSVC 下的 DLL 不会导出任何符号，因此配置期会直接
> 报错（`KAIRO_BUILD_SHARED=ON` + MSVC）。Windows 打包当前仅提供静态库：
> 使用默认的 Visual Studio（MSVC）生成器时，下文所有打包命令都需要传
> `-BuildShared:$false` 跳过动态库，直到导出宏补齐。详见 [BUILD.md](BUILD.md)。

---

## 快速开始

### 一键构建和打包

使用提供的 PowerShell 脚本一键完成构建和打包（MSVC 下仅构建静态库，见页首限制）：

```powershell
.\scripts\build_and_package_windows.ps1 -BuildShared:$false
```

这将：
1. 构建静态库（Release 模式）
2. 打包成发行版本（ZIP 格式）

v0.5.0 起，推送 `v*` tag 会触发 `.github/workflows/release.yml` 在
`windows-latest` runner 上构建 x64 静态库发行包（`kairo-<版本>-windows-x64.zip`，
CPU-only）并附到 GitHub Release；本地可用同一脚本复现。

### 自定义构建选项

```powershell
.\scripts\build_and_package_windows.ps1 `
    -Version "0.6.0" `
    -BuildType "Release" `
    -Generator "" `
    -Architecture "x64" `
    -BuildStatic:$true `
    -BuildShared:$false
```

**参数说明：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-Version` | `0.6.0` | 版本号，用于打包命名 |
| `-BuildType` | `Release` | 构建类型（Release/Debug） |
| `-Generator` | 空（自动选择） | CMake 生成器；留空跟随 CMake 默认（自动适配本机/runner 安装的 VS，例如 VS 2022 或 VS 2026） |
| `-Architecture` | `x64` | 目标架构（x64/x86） |
| `-Arch` | 环境变量回退 | 打包名中的架构标识（`x64`/`arm64`）；留空时回退到 `$env:PROCESSOR_ARCHITECTURE` |
| `-BuildStatic` | `$true` | 是否构建静态库 |
| `-BuildShared` | `$true` | 是否构建动态库 |
| `-BuildTests` | `$false` | 是否构建测试 |
| `-BuildExamples` | `$false` | 是否构建示例 |
| `-BuildDir` | `build_windows` | 构建目录 |
| `-OutputDir` | `dist` | 打包输出目录 |

---

## 分步操作

### 步骤 1: 构建库

使用构建脚本分别构建静态库和动态库：

```powershell
# 构建静态库（MSVC 下的推荐方式）
.\scripts\build_windows.ps1 -BuildType Release -BuildShared:$false

# 仅构建动态库（MSVC 下配置期直接报错，见页首限制）
.\scripts\build_windows.ps1 -BuildType Release -BuildStatic:$false
```

### 步骤 2: 打包发行版本

构建完成后，使用打包脚本创建发行包：

```powershell
.\scripts\package_windows.ps1 -Version "0.6.0"
```

打包脚本会：
- 复制静态库和动态库的安装文件
- 复制文档（README.md, LICENSE, CHANGELOG.md）
- 创建使用说明（USAGE.md）
- 生成 ZIP 压缩包

---

## 手动构建（不使用脚本）

### 构建静态库

```powershell
# 配置
cmake -B build_static `
    -G "Visual Studio 17 2022" `
    -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DKAIRO_BUILD_SHARED=OFF `
    -DKAIRO_BUILD_TESTS=OFF `
    -DKAIRO_BUILD_EXAMPLES=OFF `
    -DCMAKE_INSTALL_PREFIX=build_static\install

# 构建
cmake --build build_static --config Release

# 安装
cmake --install build_static --config Release
```

### 构建动态库

> **Windows (MSVC) 限制**：`KAIRO_BUILD_SHARED=ON` + MSVC 会在配置期直接报错
> （库尚未声明 `dllexport` 注解，DLL 不会导出任何符号），以下命令在当前实现下
> 无法完成配置。Windows 打包请使用上方静态库流程，直到导出宏补齐；详见页首
> 限制与 [BUILD.md](BUILD.md)。

```powershell
# 配置
cmake -B build_shared `
    -G "Visual Studio 17 2022" `
    -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DKAIRO_BUILD_SHARED=ON `
    -DKAIRO_BUILD_TESTS=OFF `
    -DKAIRO_BUILD_EXAMPLES=OFF `
    -DCMAKE_INSTALL_PREFIX=build_shared\install

# 构建
cmake --build build_shared --config Release

# 安装
cmake --install build_shared --config Release
```

---

## 打包目录结构

打包后的目录结构如下：

```
kairo-0.6.0-windows-x64/
├── static/                    # 静态库
│   ├── lib/
│   │   ├── kairo.lib      # 静态库文件
│   │   └── cmake/
│   │       └── kairo/     # CMake 配置文件
│   └── include/
│       └── kairo/         # 头文件
├── shared/                    # 动态库
│   ├── bin/
│   │   └── kairo.dll      # 动态库文件（运行时）
│   ├── lib/
│   │   ├── kairo.lib      # 导入库（链接时）
│   │   └── cmake/
│   │       └── kairo/     # CMake 配置文件
│   └── include/
│       └── kairo/         # 头文件
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

```powershell
cmake -B build -DCMAKE_PREFIX_PATH=path\to\kairo-0.6.0-windows-x64\static
```

3. 在项目的 `CMakeLists.txt` 中：

```cmake
find_package(kairo REQUIRED)
target_link_libraries(your_target PRIVATE kairo::kairo)
```

### 使用动态库

1. 解压发行包
2. 在 CMake 配置时设置路径：

```powershell
cmake -B build -DCMAKE_PREFIX_PATH=path\to\kairo-0.6.0-windows-x64\shared
```

3. 在项目的 `CMakeLists.txt` 中：

```cmake
find_package(kairo REQUIRED)
target_link_libraries(your_target PRIVATE kairo::kairo)
```

4. **重要**: 确保 `kairo.dll` 在运行时可用：
   - 将 `kairo.dll` 复制到可执行文件目录
   - 或将包含 `kairo.dll` 的目录添加到 PATH 环境变量

---

## 系统要求

- **操作系统**: Windows 10 或更高版本
- **编译器**: Visual Studio 2019 或更高版本（MSVC 14.0+）
- **CMake**: 3.16 或更高版本
- **C++ 标准**: C++20

---

## 常见问题

### Q: 如何选择 Visual Studio 版本？

A: 使用 `-Generator` 参数指定：

```powershell
# Visual Studio 2019
.\scripts\build_windows.ps1 -Generator "Visual Studio 16 2019"

# Visual Studio 2022
.\scripts\build_windows.ps1 -Generator "Visual Studio 17 2022"
```

### Q: 如何构建 x86 版本？

A: 使用 `-Architecture` 参数：

```powershell
.\scripts\build_windows.ps1 -Architecture "Win32"
```

### Q: 构建失败，提示找不到 CMake？

A: 确保 CMake 已安装并在 PATH 环境变量中。可以运行 `cmake --version` 验证。

### Q: 使用动态库时提示找不到 kairo.dll？

A: 确保 `kairo.dll` 在以下位置之一：
- 可执行文件所在目录
- 系统 PATH 环境变量中的目录
- 当前工作目录

### Q: 如何同时构建 Debug 和 Release 版本？

A: 分别运行两次构建：

```powershell
# Debug 版本
.\scripts\build_windows.ps1 -BuildType Debug -BuildDir build_windows_debug

# Release 版本
.\scripts\build_windows.ps1 -BuildType Release -BuildDir build_windows_release
```

---

## 验证构建结果

构建完成后，可以验证关键文件：

### 静态库
- `build_windows/static/install/lib/kairo.lib` - 静态库文件
- `build_windows/static/install/include/kairo/` - 头文件目录

### 动态库
- `build_windows/shared/install/bin/kairo.dll` - 动态库文件
- `build_windows/shared/install/lib/kairo.lib` - 导入库文件
- `build_windows/shared/install/include/kairo/` - 头文件目录

---

## 发布检查清单

在发布前，请确认：

- [ ] 版本号正确（在 `CMakeLists.txt` 和打包脚本中）
- [ ] 静态库已成功构建（MSVC 下动态库不可用，见页首限制）
- [ ] 所有头文件都已包含在打包中
- [ ] CMake 配置文件已正确生成
- [ ] 文档文件（README.md, LICENSE, CHANGELOG.md）已包含
- [ ] 使用说明（USAGE.md）已生成
- [ ] ZIP 压缩包已创建
- [ ] 在测试环境中验证了静态库和动态库的使用

---

## 相关文档

- [BUILD.md](BUILD.md) - 通用构建说明
- [API.md](API.md) - API 使用文档
- [README.md](../README.md) - 项目说明
