---
title: Build and Install
description: Build Kairo from source and include the tutorial examples.
---

# Build and Install

## Goal

Create a Release build without GPU or real-time privilege requirements, then compile the tutorial examples.

## Build from source

Run these commands from the repository root:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKAIRO_BUILD_EXAMPLES=ON -DKAIRO_ENABLE_GPU=OFF
cmake --build build
```

Linux requires CMake 3.16+ and a C++20 compiler. On Windows, use Visual Studio 2019+ with MSVC. Android uses NDK r26c/r28b and is CPU-only in this stage:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r26c
scripts/build_android.sh --abi arm64-v8a --api 21
```

For Android packaging, AGP integration, and `c++_shared`, see [PACKAGE_ANDROID.md](https://github.com/Linductor-alkaid/kairo/blob/master/docs/PACKAGE_ANDROID.md). CUDA, OpenCL, and real-time privileges are not needed for this first path.

## Use it from your project

Before installation, add the repository as a subdirectory:

```cmake
set(KAIRO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KAIRO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/executor)
target_link_libraries(myapp PRIVATE kairo::executor)
```

After installation, pass the installation prefix to CMake:

```bash
cmake --install build --prefix /opt/executor
cmake -B build-consumer -DCMAKE_PREFIX_PATH=/opt/executor
```

Then use:

```cmake
find_package(kairo REQUIRED)
target_link_libraries(myapp PRIVATE kairo::executor)
```

For packaging and installation details, read [`docs/BUILD.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/BUILD.md).

## Verify the tutorial example

```bash
./build/examples/tutorial/tutorial_01_first_task
```

On Windows, run the corresponding `.exe` from the build directory. The first line is `answer=42`, followed by a deliberately caught exception. This confirms that both results and task failures return to the caller.

To run all tutorial smoke tests:

```bash
cmake --build build
ctest --test-dir build -L tutorial --output-on-failure
```

For a Linux, Windows, or Android production deployment, use the [platform deployment checklist](/en/reliability/platform-deployment) before tuning permissions or affinity.

Next, [run your first task](/en/quick-start/first-task).
