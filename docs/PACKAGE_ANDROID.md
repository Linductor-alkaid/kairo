# Android 打包与集成指南

本文档说明如何用 NDK + CMake 构建 kairo 的 Android 静态库/共享库，并将其接入
AGP（Android Gradle Plugin）工程。一期 Android 能力边界：**CPU-only、调度调优
best-effort、不承诺 GPU 与硬实时**。

---

## 1. 环境要求

| 组件 | 要求 |
| --- | --- |
| NDK | r26c 或 r28b（CI 使用双版本；建议 r26c 作为低版本基线） |
| CMake | 3.16+；Android 交叉编译建议 3.28+ |
| Android API | `ANDROID_PLATFORM=android-21` 起 |
| ABI | arm64-v8a 必选；armeabi-v7a / x86 / x86_64 为兼容目标 |
| STL | `c++_static`（默认，单二进制便利）或 `c++_shared`（多模块共享 libc++） |
| GPU | 一期关闭：`KAIRO_ENABLE_GPU=OFF` |

---

## 2. 使用 `scripts/build_android.sh`

脚本会按 ABI 构建 static / shared 并安装到 `build-android/<abi>/<variant>/install`。

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r26c

# 默认：arm64-v8a + x86_64，API 21，static + shared，CPU-only
scripts/build_android.sh

# 自定义
scripts/build_android.sh \
    --abi arm64-v8a,x86_64 \
    --api 21 \
    --build-static true \
    --build-shared false \
    --build-tests true
```

输出：

```text
build-android/arm64-v8a/static/install/
├── include/kairo/...
└── lib/libkairo.a

build-android/arm64-v8a/shared/install/
├── include/kairo/...
└── lib/libkairo.so
```

`--build-tests true` 会额外构建 `tests/android_smoke` 等 standalone 测试；测试设备侧
运行见 `scripts/run_android_tests.sh` 与 `scripts/capture_android_device_info.sh`。

---

## 3. 静态库与共享库

| 项 | static + c++_static | shared + c++_shared |
| --- | --- | --- |
| APK 内额外产物 | 无 | 需要随 APK/AAR 携带 `libc++_shared.so` |
| 多模块混用 | 每个 `.so` 都包含 libc++ 代码，注意 ODR | 所有模块共享一份 libc++，异常/RTTI 可跨模块 |
| kairo 自身 | 链接进业务 `.so` | 独立 `libkairo.so` |
| 推荐场景 | 单 native 模块或 smoke 工具 | 多 native 模块共享 kairo |

Android CMake 默认 STL 由 NDK toolchain 决定。静态库目标不会把 `librt` / `libatomic`
错误导出；共享库目标只依赖 `libc.so` / `libm.so` / `libdl.so`。

---

## 4. CMake 消费者集成（`find_package`）

先构建并安装：

```bash
ANDROID_NDK_HOME=/path/to/ndk scripts/build_android.sh \
    --abi arm64-v8a --api 21 --build-shared true
```

消费者配置：

```bash
cmake -S app -B app/build \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-21 \
    -DCMAKE_FIND_ROOT_PATH="/path/to/build-android/arm64-v8a/shared/install;$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot" \
    -DCMAKE_BUILD_TYPE=Release
```

关键点：Android toolchain 会把 `CMAKE_FIND_ROOT_PATH` 限制到 NDK sysroot，因此必须
显式把 kairo 安装目录加入 root path；也可以直接传 `-Dexecutor_DIR=/path/to/install/lib/cmake/kairo`。

消费者 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(app LANGUAGES CXX)

find_package(kairo REQUIRED)

add_library(app SHARED app_jni.cpp)
target_link_libraries(app PRIVATE kairo::kairo)
```

---

## 5. AGP `externalNativeBuild` 接入

推荐通过 Gradle 管理 NDK 与 ABI：

```groovy
android {
    defaultConfig {
        ndk {
            abiFilters += ["arm64-v8a", "x86_64"]
        }
        externalNativeBuild {
            cmake {
                cppFlags += ["-std=c++20"]
                arguments += ["-DKAIRO_BUILD_TESTS=OFF",
                              "-DKAIRO_BUILD_EXAMPLES=OFF",
                              "-DKAIRO_ENABLE_GPU=OFF"]
            }
        }
    }
    externalNativeBuild {
        cmake {
            path file("CMakeLists.txt")
        }
    }
}
```

CMake 中可以直接 `add_subdirectory`：

```cmake
set(KAIRO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KAIRO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(KAIRO_ENABLE_GPU OFF CACHE BOOL "" FORCE)

add_subdirectory(path/to/kairo)

add_library(app SHARED app_jni.cpp)
target_link_libraries(app PRIVATE kairo::kairo)
```

---

## 6. 共享 libc++ 打包

当 native 模块使用 `c++_shared` 时，必须把 NDK 中的 libc++ 打进 APK/AAR：

```bash
NDK_LIBCXX="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib"

# arm64-v8a
cp "$NDK_LIBCXX/aarch64-linux-android/libc++_shared.so" \
    app/src/main/jniLibs/arm64-v8a/

# x86_64
cp "$NDK_LIBCXX/x86_64-linux-android/libc++_shared.so" \
    app/src/main/jniLibs/x86_64/
```

避免同一进程内混用 `c++_static` 和 `c++_shared`；跨 native 模块抛异常或使用 RTTI 时，
统一使用 `c++_shared` 更安全。

---

## 7. JNI 生命周期与显式 shutdown

Android 不保证进程退出时执行 C++ 静态析构。JNI 层应显式关闭单例 kairo：

```cpp
#include <jni.h>
#include <kairo/kairo.hpp>

extern "C" JNIEXPORT void JNICALL
Java_com_example_app_NativeExecutor_shutdown(JNIEnv*, jobject) {
    auto& kairo = kairo::Executor::instance();
    kairo.shutdown();  // 停止线程池、实时线程、Blocking I/O worker 并 join
}
```

建议调用点：

- `Activity.onDestroy()` / Service 销毁路径（业务主动 shutdown）
- `JNI_OnUnload()`（兜底，但 Android 不保证一定回调）

不要在 RT 线程或 kairo worker 内直接调用 shutdown；如果必须从 worker 内停止自身，
先检查对应执行器的 self-stop 契约。

---

## 8. Prefab / AAR（可选发布形态）

若要把 kairo 作为 AAR 分发给 AGP 消费者，建议结构：

```text
kairo-android-0.5.0.aar
├── prefab/modules/kairo/
│   ├── module.json
│   ├── include/kairo/...
│   └── libs/
│       ├── android.arm64-v8a/libkairo.so
│       └── android.x86_64/libkairo.so
└── jni/
    ├── arm64-v8a/libc++_shared.so
    └── x86_64/libc++_shared.so
```

`prefab/modules/kairo/module.json` 示例：

```json
{
  "schema_version": 2,
  "name": "kairo",
  "dependencies": [],
  "export_library_names": ["kairo"]
}
```

一期不把 tests / examples 打入 AAR。正式发布 AAR 前还需补充 `prefab` 包版本策略、
`libc++_shared` 冲突策略和发布签名流程；当前仓库先提供 NDK CMake 安装包作为事实源。

---

## 9. 验证

```bash
# 交叉编译
ANDROID_NDK_HOME=/path/to/ndk scripts/build_android.sh --abi arm64-v8a,x86_64

# 官方模拟器/真机运行 standalone 测试
PATH=/path/to/platform-tools:$PATH ANDROID_SERIAL=<serial> \
    scripts/capture_android_device_info.sh \
    --test-dir build-android/arm64-v8a/static/tests \
    --soak-seconds 600
```

设备矩阵与验证结果见 [docs/performance/android_a3_validation.md](performance/android_a3_validation.md)。
big.LITTLE 真机验证在发布前为强制 gate，见 [docs/RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md)。
