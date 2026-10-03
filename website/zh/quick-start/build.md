---
title: 构建与安装
description: 从源码构建 Kairo，并把教程示例加入构建。
---

# 构建与安装

## 学习目标

完成一个无 GPU、无实时权限要求的 Release 构建，并编译教程示例。

## 从源码构建

在仓库根目录执行：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKAIRO_BUILD_EXAMPLES=ON -DKAIRO_ENABLE_GPU=OFF
cmake --build build
```

Linux 需要 CMake 3.16+ 和 C++20 编译器；Windows 使用 Visual Studio 2019+ / MSVC。Android 使用 NDK r26c/r28b，一期为 CPU-only：

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r26c
scripts/build_android.sh --abi arm64-v8a --api 21
```

Android 构建、AGP 接入和 `c++_shared` 打包见 [PACKAGE_ANDROID.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/PACKAGE_ANDROID.md)。首次路径不要求 CUDA、OpenCL 或实时权限。

## 加入自己的工程

未安装时，将仓库作为子目录加入项目：

```cmake
set(KAIRO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KAIRO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/executor)
target_link_libraries(myapp PRIVATE kairo::executor)
```

安装后，配置消费者工程时提供安装前缀：

```bash
cmake --install build --prefix /opt/executor
cmake -B build-consumer -DCMAKE_PREFIX_PATH=/opt/executor
```

消费者 `CMakeLists.txt` 使用：

```cmake
find_package(kairo REQUIRED)
target_link_libraries(myapp PRIVATE kairo::executor)
```

完整安装和打包说明见 [`docs/BUILD.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/BUILD.md)。

## 验证教程示例

```bash
./build/examples/tutorial/tutorial_01_first_task
```

Windows 下可通过构建目录中的同名 `.exe` 运行。预期首先看到 `answer=42`，然后看到已捕获的示例异常；这说明结果和任务失败都回到了调用方。

若要运行全部教程 smoke test，先构建全部示例：

```bash
cmake --build build
ctest --test-dir build -L tutorial --output-on-failure
```

## 常见错误

- 忘记 `-DKAIRO_BUILD_EXAMPLES=ON`：教程可执行文件不会生成。
- 把 GPU 依赖带入首次构建：首个教程不需要 GPU，先显式设为 `OFF` 可减少环境差异。
- 使用多配置 Windows 生成器时：在构建和 CTest 命令中补充 `--config Release`。
- 只构建不运行：下一页用输出确认 `future.get()` 的行为。

## 下一步

进入[第一个任务](/zh/quick-start/first-task)。准备部署到正式 Linux、Windows 或 Android 环境时，使用[平台部署核对](/zh/reliability/platform-deployment)验证权限、亲和性和实际运行状态。
