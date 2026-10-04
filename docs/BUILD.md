# Kairo 构建与安装说明

本文档说明如何配置、构建、测试、安装 kairo 库，以及如何在其他项目通过 `find_package(kairo)` 集成。

---

## 1. 环境要求

- **C++20** 编译器（GCC 10+、Clang 10+ 等）
- **CMake** 3.16 或更高
- **Linux**：`pthread`、`rt`（一般系统已提供）
- **Android**：NDK r26c / r28b（交叉编译建议 CMake 3.28+），一期为 CPU-only

---

## 2. 配置选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `KAIRO_BUILD_TESTS` | `ON` | 是否构建测试 |
| `KAIRO_BUILD_EXAMPLES` | `OFF` | 是否构建示例 |
| `KAIRO_BUILD_SHARED` | `OFF` | 是否构建动态库（`OFF` 时构建静态库；MSVC 下配置期报错，见 [§3.3](#33-构建动态库)） |
| `KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD` | `OFF` | 是否启用 Linux 诊断构建中显式 guard 实时路径的分配跟踪（仅 Linux 生效，其他平台配置时仅告警） |
| `KAIRO_ENABLE_COVERAGE` | `OFF` | 是否启用代码覆盖率（gcov/lcov，见 [COVERAGE.md](COVERAGE.md)） |
| `KAIRO_ENABLE_GPU` | `ON`（Android 默认 `OFF`） | 是否启用 GPU 支持 |
| `KAIRO_ENABLE_CUDA` | `ON`（Android 默认 `OFF`） | 是否启用 CUDA 支持（需 `KAIRO_ENABLE_GPU=ON`） |
| `KAIRO_ENABLE_OPENCL` | `OFF` | 是否启用 OpenCL 支持（需 `KAIRO_ENABLE_GPU=ON`） |
| `KAIRO_LOCKFREE_QUEUE` | `OFF` | 是否将 worker 本地队列实现替换为 `LockFreeWorkerQueue`（定义 `USE_LOCKFREE_WORKER_QUEUE`） |
| `KAIRO_ENABLE_TSAN` | `OFF` | 是否启用 ThreadSanitizer（`-fsanitize=thread`；仅 GCC/Clang，其他编译器配置时仅告警） |

**TSAN 构建下的测试口径**：CI 的 thread-sanitizer job 只运行并发相关的测试子集（见 `.github/workflows/c-cpp.yml`），不含 benchmark。benchmark_* 测试带 `RUN_SERIAL` 属性（ctest 独占调度，避免并行负载放大延迟分位数抖动），且其延迟断言在 sanitizer 构建下自动豁免——插桩放大概率 10-100 倍，延迟数字不作为 TSAN 门禁。

---

## 3. 配置与构建

### 3.1 默认构建（静态库 + 测试）

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 3.2 构建示例

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKAIRO_BUILD_EXAMPLES=ON
cmake --build build
```

完整生命周期监控示例不依赖 GPU，可直接运行：

```bash
./build/examples/lifecycle_snapshot
```

该示例会在单线程任务积压时读取 `Executor::get_snapshot()`，观察任务异常后的
失败摘要和稳定文本导出，并在关闭后确认 `Stopped` 生命周期。它是低频诊断示例，
不应移植到 realtime cycle thread。

### 3.3 构建动态库

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKAIRO_BUILD_SHARED=ON
cmake --build build
```

> **Windows (MSVC) 限制**：动态库当前仅支持 GCC/Clang（Linux/Android）。库尚未
> 声明 `dllexport` 注解，MSVC 下的 DLL 不会导出任何符号，因此配置期会直接
> 报错（`KAIRO_BUILD_SHARED=ON` + MSVC）。Windows 请使用默认静态库，直到
> 导出宏补齐。

### 3.4 Android 交叉编译

Android 一期为 CPU-only；GPU 默认关闭。先准备 NDK，再调用专用脚本：

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r26c

# arm64-v8a + x86_64，API 21，static + shared
scripts/build_android.sh

# 只构建 arm64-v8a 静态库与 standalone 测试
scripts/build_android.sh \
    --abi arm64-v8a \
    --api 21 \
    --build-static true \
    --build-shared false \
    --build-tests true
```

产物位于 `build-android/<abi>/<static|shared>/install`。NDK/AGP/`c++_shared` 打包说明见
[PACKAGE_ANDROID.md](PACKAGE_ANDROID.md)，设备测试脚本见 `scripts/run_android_tests.sh` 与
`scripts/capture_android_device_info.sh`。

> Android 不支持 CUDA，OpenCL 也不在一期范围。Android 上的 priority / affinity /
> mlock / timer slack 均为 best-effort，不承诺硬实时。

### 3.5 关闭测试

```bash
cmake -B build -DKAIRO_BUILD_TESTS=OFF
cmake --build build
```

### 3.6 启用 GPU 支持

```bash
# 启用 CUDA（NVIDIA GPU）
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKAIRO_ENABLE_GPU=ON -DKAIRO_ENABLE_CUDA=ON
cmake --build build

# 启用 OpenCL（Intel/AMD/NVIDIA GPU）
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKAIRO_ENABLE_GPU=ON -DKAIRO_ENABLE_OPENCL=ON
cmake --build build

# 同时启用 CUDA 和 OpenCL
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKAIRO_ENABLE_GPU=ON -DKAIRO_ENABLE_CUDA=ON -DKAIRO_ENABLE_OPENCL=ON
cmake --build build
```

查询系统 GPU 设备：

```bash
./build/examples/gpu_device_query
```

GPU 环境配置详见 [setup/opencl_setup.md](setup/opencl_setup.md)。

### 3.7 指定安装前缀（安装时使用）

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
```

---

## 4. 运行测试

```bash
ctest --test-dir build
```

或带输出：

```bash
ctest --test-dir build --output-on-failure
```

按标签过滤（若已配置）：

```bash
ctest --test-dir build -L "unit|integration" --output-on-failure
```

---

## 5. 安装

安装到 `CMAKE_INSTALL_PREFIX`（默认一般为 `/usr/local`）：

```bash
cmake --install build
```

指定前缀：

```bash
cmake --install build --prefix /opt/kairo
```

安装内容包含：

- **头文件**：`<prefix>/include/kairo/`（如 `executor.hpp`、`scheduling.hpp`、`scheduler.hpp`、`config.hpp`、`types.hpp` 等）
- **库文件**：`<prefix>/lib/` 或 `<prefix>/lib64/`（`libkairo.a` 或 `libkairo.so`）
- **CMake 配置**：`<prefix>/lib/cmake/kairo/`（`kairoConfig.cmake`、`kairoConfigVersion.cmake`、`kairoTargets.cmake` 等），供 `find_package(kairo)` 使用

---

## 6. 在其他项目中使用（find_package）

### 6.1 安装后使用

确保安装路径在 CMake 的搜索路径中。若安装到自定义前缀，可设置：

```bash
export CMAKE_PREFIX_PATH=/opt/kairo
```

或配置时传入：

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/opt/kairo
```

消费者项目 `CMakeLists.txt` 示例：

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp LANGUAGES CXX)

find_package(kairo REQUIRED)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE kairo::kairo)
```

### 6.2 未安装：add_subdirectory

若将 kairo 作为子目录加入当前项目：

```cmake
add_subdirectory(path/to/kairo)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE kairo::kairo)
```

可根据需要关闭测试、示例等：

```cmake
set(KAIRO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KAIRO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/kairo)
```

---

## 7. 代码覆盖率

使用 gcov/lcov 生成覆盖率报告。详见 [COVERAGE.md](COVERAGE.md)。简要步骤：

```bash
cmake -B build -DKAIRO_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -L "unit|integration" --output-on-failure
# 随后在 build 目录运行 lcov/genhtml，或使用 scripts/run_coverage.sh
./scripts/run_coverage.sh
```

---

## 8. 常见问题

| 问题 | 处理 |
|------|------|
| `find_package(kairo)` 找不到 | 确认已 `cmake --install`，且 `CMAKE_PREFIX_PATH` 包含安装前缀；或使用 `add_subdirectory`。 |
| 链接错误（如 `pthread`） | kairo 通过 `Threads::Threads` 拉取 `pthread`，确保消费者项目同样使用 `kairo::kairo` 而不是手动 `-lpthread` 覆盖。 |
| 找不到 kairo 头文件 | 使用 `target_link_libraries(… kairo::kairo)`，勿手动添加 `-I`；`kairo::kairo` 已携带 `INTERFACE_INCLUDE_DIRECTORIES`。 |
| 静态库与动态库混用 | 同一进程内链接的 kairo 应与主程序同类型（全静态或全动态），避免符号重复或加载冲突。 |

---

## 9. 准备发布包（源码归档）

以当前工程创建源码归档，便于分发或发布：

```bash
git archive --format=tar.gz --prefix=kairo-0.6.0/ -o kairo-0.6.0.tar.gz HEAD
```

或仅打包 `include/`、`src/`、`cmake/`、`examples/`、`tests/`、`CMakeLists.txt`、`README.md`、`CHANGELOG.md`、`docs/` 等必要目录与文件（按需调整）。解压后按 [§3](#3-配置与构建) 配置与构建即可。

---

## 10. 构建目录结构速览

```
build/
├── libkairo.a（或 libkairo.so）
├── kairo 可执行目标（若启用示例）
├── test_* 测试可执行文件
└── ...
```

安装后：

```
<prefix>/
├── include/kairo/
│   ├── executor.hpp
│   ├── scheduling.hpp
│   ├── scheduler.hpp
│   ├── config.hpp
│   ├── types.hpp
│   ├── interfaces.hpp
│   └── executor_manager.hpp
├── lib/libkairo.a（或 lib64/）
└── lib/cmake/kairo/
    ├── kairoConfig.cmake
    ├── kairoConfigVersion.cmake
    └── kairoTargets.cmake
```
