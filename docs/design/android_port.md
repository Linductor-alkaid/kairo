# Android 适配方案


> [!NOTE]
> 历史设计快照：Android 移植一期已交付。构建选项现为 `KAIRO_*` 系列。

> 状态：**一期已交付**（CPU-only 交叉编译 CI、`executor::StopToken` 兼容层、
> best-effort 调度语义、打包与集成文档均已合入；实施记录见
> [`docs/todolists/android_port_plan.md`](../archive/todolists/android_port_plan.md)）。
> 未完成：arm64 真机 smoke test 与 big.LITTLE 验证（见 CHANGELOG"Android 适配一期"
> 的验证边界）。本文最初为评审期设计稿，§1 的三个阻塞点已在一期解决，保留为决策记录。
> 本文只描述 executor 核心库如何适配 Android；具体 App、JNI 业务、协议或硬件接入不属于本项目。

## 1. 结论

**Android 适配可行，但必须先完成三个确定阻塞点的改造，再按“CPU-only、best-effort 调度、真机验证”的边界落地。**

~~当前仓库不能直接交叉编译。~~（一期已解决：`__ANDROID__` 编译路径、StopToken 兼容层与 NDK CI 均已落地，下述阻塞点为评审期记录。）当时用 NDK r26c / r28b 实际验证，定位到以下问题：

1. Android libc++ 对 `std::stop_token` / `std::jthread` 支持不稳定，当前公开头 `blocking_io.hpp` 直接依赖它们。
2. bionic 没有 glibc 扩展 `pthread_setaffinity_np` / `pthread_getaffinity_np`。
3. CMake 把 Android 当成普通 UNIX/Linux，链接了不存在的 `librt`，并错误导出 `libatomic` 依赖。

修复以上问题后，CPU-only 静态库、共享库和现有 examples 均可交叉编译。**GPU、硬实时和无锁队列在 ARM 弱内存序上的正确性必须单独验证，不能随编译通过自动放行。**

---

## 2. 目标平台与一期范围

### 2.1 目标平台

| 项目 | 一期值 | 说明 |
| --- | --- | --- |
| NDK | r26c、r28b 双基线 | 已实测；r23~r25 需在 CI 中补充，不能假设相同 |
| minSdkVersion / ANDROID_PLATFORM | 21 | 所需 pthread/sched/mlock/prctl 符号在 API 21 已存在 |
| 主 ABI | arm64-v8a | 必须 |
| 兼容 ABI | armeabi-v7a、x86、x86_64 | 已交叉编译通过，作为兼容构建目标 |
| 后续 ABI | riscv64 | 仅 NDK r28b + API 35 验证编译通过，暂不承诺运行 |
| STL | c++_shared / c++_static | 共享库和跨模块异常场景必须统一 |
| CMake | 项目保持 3.16+ 下限；Android CI 使用 3.28+ | 实际 CI 以工具链可用版本为准 |
| GPU | 一期关闭 | `EXECUTOR_ENABLE_GPU=OFF` |

### 2.2 一期承诺

- 普通线程池异步任务。
- 无锁低延迟任务执行器（编译与功能路径）。
- 周期实时线程执行器（best-effort，不承诺硬实时）。
- 长期 Blocking I/O worker（可中断、可 join 的生命周期语义）。
- `executor::comm` 进程内通信组件。
- 监控、任务图和生命周期状态查询。
- 静态库、共享库、NDK CMake 集成和基础安装。

### 2.3 一期不承诺

- CUDA；Android 上基本无 CUDA runtime。
- OpenCL 设备兼容列表；仅保留后续可扩展点。
- `SCHED_FIFO`、CPU affinity、`mlockall` 一定生效。Android 普通 App 通常无权限，这些保持 **best-effort + 状态可观测**。
- 弱内存序下无锁队列“经充分验证”。必须先完成真机/模拟器压力测试。
- App 生命周期自动管理；JNI 层必须显式调用 shutdown。

---

## 3. 实测基线

### 3.1 当前仓库未修改时的编译结果

| NDK | ABI / API | 配置 | 结果 |
| --- | --- | --- | --- |
| r26c | arm64-v8a / 21 | CPU-only | `fatal error: 'stop_token' file not found` |
| r28b | arm64-v8a / 21 | CPU-only | `<stop_token>` 存在但 `std::jthread` 默认未启用；继续报 pthread affinity 缺失 |
| r28b | arm64-v8a / 21 | CPU-only + `-fexperimental-library` | `pthread_setaffinity_np` / `pthread_getaffinity_np` 未声明 |
| r28b | arm64-v8a / 21 | 修复 affinity 后构建共享库 | `ld.lld: error: unable to find library -lrt` |

### 3.2 临时修正后的通过项

在**本地临时副本**中完成以下最小修正后，交叉编译通过：

- affinity 改为 `sched_setaffinity(gettid(), ...)` / `sched_getaffinity(gettid(), ...)`。
- CMake Android 分支不再链接 `rt` / `atomic`。
- stop_token 兼容层或 `-fexperimental-library`。

通过矩阵：

| 配置 | 结果 |
| --- | --- |
| arm64-v8a / armeabi-v7a / x86 / x86_64，API 21，static + shared，NDK r28b | 通过 |
| arm64-v8a / armeabi-v7a，API 21，NDK r26c + 自有 stop_token 兼容层 | 通过 |
| riscv64，API 35，NDK r28b | 编译通过 |
| `EXECUTOR_BUILD_EXAMPLES=ON`，CPU examples + tutorial | 全部通过 |
| 安装后 `find_package(executor)` 消费者 | 通过（需正确设置 `CMAKE_FIND_ROOT_PATH` 或 `executor_DIR`） |

复现命令：

```bash
NDK=/path/to/android-ndk-r28b

cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-21 \
  -DEXECUTOR_BUILD_TESTS=OFF \
  -DEXECUTOR_BUILD_EXAMPLES=OFF \
  -DEXECUTOR_ENABLE_GPU=OFF \
  -DEXECUTOR_ENABLE_CUDA=OFF \
  -DEXECUTOR_ENABLE_OPENCL=OFF \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-android -j
```

> 注意：上述命令在当前仓库仍会失败；它是一期完成后的验收命令。

---

## 4. 平台差异与设计决策

### 4.1 `std::stop_token` / `std::jthread`

**影响文件：**
- `include/executor/blocking_io.hpp`
- `src/executor/blocking_io_executor.hpp` / `.cpp`
- `examples/tutorial/12_blocking_io_worker.cpp`
- 相关测试

**推荐方案：引入 `executor::StopToken`，桌面平台保持 ABI 兼容。**

新增 `include/executor/stop_token.hpp`：

```cpp
namespace executor {

#if defined(__ANDROID__) && !defined(__cpp_lib_jthread)
class StopToken;
class StopSource;
namespace detail { class JThread; }
#else
using StopToken = std::stop_token;
#endif

} // namespace executor
```

要点：

1. 桌面平台 `executor::StopToken` 是 `std::stop_token` 的别名，现有 `run(std::stop_token)` override 的源码和 ABI 均不受影响。
2. Android 且 libc++ 未启用 jthread 时，使用自有最小实现：共享 `atomic<bool>` 停止状态、可复制的 `StopToken`、可移动的 `StopSource` 和 `detail::JThread`。
3. `detail::JThread` 语义对齐 `std::jthread`：析构时 `request_stop()` 后 `join()`；禁止拷贝；移动赋值时先收敛旧线程。
4. `IBlockingIoWorker::run()` 改为接收 `executor::StopToken`；项目内示例和测试统一迁移，用户在新 Android 代码中同样使用 `executor::StopToken`。
5. **不采用**“要求 NDK r28b + `-fexperimental-library` 传播到所有消费者”的方案：公共头不能长期依赖 experimental 特性，且 r26c 完全无法使用。
6. **不采用**在 `std` 命名空间中注入 polyfill：避免与未来 NDK 头文件冲突和未定义行为。

### 4.2 bionic 的 CPU affinity API

`src/executor/util/thread_utils.cpp` 当前在 `__linux__` 分支调用：

```cpp
pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset);
pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
```

这两个是 glibc 扩展，bionic 不提供。Android 分支改为：

```cpp
#include <unistd.h> // gettid

sched_setaffinity(gettid(), sizeof(cpu_set_t), &cpuset);
sched_getaffinity(gettid(), sizeof(cpu_set_t), &cpuset);
```

说明：

- `gettid()` 在 API 21 可用。
- `sched_setaffinity` / `sched_getaffinity` 在 API 21 的 bionic 中可用。
- 普通 App 仍可能因 cgroup/SELinux 只允许绑到 App cpuset 内的核，调用失败时保持当前“非致命 + 状态字段记录”的语义。
- 现有 `set_thread_priority()`、`pthread_setschedparam`、`pthread_setname_np`、`prctl(PR_SET_TIMERSLACK)`、`mlockall` 在 API 21 均有符号；保留 best-effort 语义。

### 4.3 CMake 平台识别与链接

当前：

```cmake
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(executor PUBLIC Threads::Threads)
    target_link_libraries(executor PUBLIC atomic)
    target_link_libraries(executor PRIVATE rt)
```

Android 也命中该分支，但 bionic 没有独立 `librt`，`atomic` 不应作为 Android 公共依赖。改为：

```cmake
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(executor PUBLIC Threads::Threads)

    if(ANDROID)
        # pthread / rt / sched 都在 bionic libc；不链接 rt，也不导出 atomic
    else()
        target_link_libraries(executor PUBLIC atomic)
        target_link_libraries(executor PRIVATE rt)
    endif()
elseif(WIN32)
    ...
endif()
```

同时：

- Android 构建默认建议 `EXECUTOR_ENABLE_GPU=OFF`；CMake 中对 `ANDROID` 默认关闭 CUDA/OpenCL 探测，或至少在 CI 和脚本中显式传入。
- 编译器警告列表按平台裁剪：NDK clang 不认识 `-Wlogical-op`、`-Wnoexcept`、`-Wstrict-null-sentinel`，会产生大量 unknown warning option。
- `EXECUTOR_ENABLE_REALTIME_ALLOCATION_GUARD` 的 CMake 判断当前是 `CMAKE_SYSTEM_NAME STREQUAL "Linux"`，Android 会落到“不支持”警告。一期保持关闭，暂不支持。

### 4.4 线程数与 cpuset 自适应

`ExecutorManager::initialize_async_executor()` 使用 `std::thread::hardware_concurrency()` 计算默认线程数，并生成 `0..hw-1` 的 affinity。Android 上：

- `hardware_concurrency()` 返回系统在线核数，不一定等于 App cpuset 可用核。
- 8 核手机可能默认创建 8 个线程，移动端能耗和缓存竞争不友好。
- `0..hw-1` 中可能有核不在 App cpuset 内，绑核调用失败。

一期调整：

```cpp
#if defined(__ANDROID__)
constexpr unsigned kAndroidDefaultMaxThreads = 4;
unsigned hw = std::thread::hardware_concurrency();
hw = std::min(hw, kAndroidDefaultMaxThreads);
#endif
```

自动 affinity 优先使用 `util::get_current_thread_affinity()` 返回的允许掩码；为空时保持 OS 自由调度，不伪造 `0..hw-1`。

### 4.5 实时调度的 Android 语义

- `SCHED_FIFO`：普通 App 通常 EPERM。`RealtimeThreadExecutor` 继续尝试并写入 `priority_applied=false`，不因调优失败拒绝任务。
- CPU affinity：受 cgroup/SELinux 限制，同上。
- `mlockall`：进程级、非 root 基本失败；默认关闭，状态字段保留。
- timer slack：`prctl(PR_SET_TIMERSLACK)` 存在，但内核/厂商策略可能忽略；不承诺精度。
- 文档必须写清：Android 上这些是 **best-effort tuning**，不是硬实时保证。`cycle_period_ns` 仍是软目标。

### 4.6 ARM 弱内存序

`src/executor/util/lockfree_queue.hpp` 已有明确注释：弱序架构上的端到端正确性需要 CI 验证。Android 主力是 ARM，这是最高运行时风险。

一期必须有设备侧验证：

- MPSC 多生产者压力。
- `push_batch_exact` / `pop_batch` 批量路径。
- reservation cancellation 与 stalled producer 恢复。
- worker queue 并发窃取。
- 长时间 soak。

编译通过不构成放行依据。

### 4.7 GPU 后端

- CUDA：Android 目标不启用；`cuda_loader` 的桌面路径和 `cudart64_*.dll` 逻辑不适用于 Android。
- OpenCL：部分 Android 设备在 `/vendor/lib64/libOpenCL.so` 提供实现，但碎片化严重；NDK 不带 OpenCL 头文件；App 对 vendor 私有库的 `dlopen` 受 namespace/SELinux 限制。
- 一期：CMake 默认关闭 GPU；API 在 `EXECUTOR_ENABLE_GPU` 未定义时保持现有 `BackendUnavailable` 行为。
- 后续：OpenCL loader 增加 Android 搜索路径并建立明确设备清单，才可开启。

### 4.8 App 生命周期与 JNI

- Android 不保证进程退出时执行所有 C++ 静态对象析构。
- 单例 `Executor::instance()` 不能作为 JNI 层“忘记 shutdown”的兜底。
- JNI / Activity / Service 生命周期应显式调用 `Executor::shutdown()`，最好在 `JNI_OnUnload` 或业务 destroy 路径。
- 共享库采用 `c++_shared` 时，APK/AAR 必须携带 `libc++_shared.so`，避免多份 libc++ 导致跨模块异常和 RTTI 失效。
- 交叉编译 `find_package(executor)` 时，CMake 默认只在 NDK sysroot 中找包；需设置 `CMAKE_FIND_ROOT_PATH` 或直接指定 `executor_DIR`。

---

## 5. 代码改动清单

### 5.1 公共头

| 文件 | 改动 |
| --- | --- |
| `include/executor/stop_token.hpp` | 新增；`executor::StopToken` 桌面别名 + Android 最小实现 |
| `include/executor/blocking_io.hpp` | `std::stop_token` 改为 `executor::StopToken` |
| `include/executor/config.hpp` | 注释补充 Android 调度语义；不新增字段 |
| `include/executor/types.hpp` | 状态字段注释补充 Android best-effort 语义 |

### 5.2 实现

| 文件 | 改动 |
| --- | --- |
| `src/executor/blocking_io_executor.hpp/.cpp` | Android 使用 `detail::JThread` / `StopSource`；桌面保持 `std::jthread` |
| `src/executor/util/thread_utils.cpp` | 增加 `__ANDROID__` affinity 实现；保留其他 Linux 路径 |
| `src/executor/executor_manager.cpp` | Android 线程数上限与 cpuset 自适应 |
| `src/executor/realtime_thread_executor.cpp` | 仅补充注释/状态语义，不改变 fallback 行为 |

### 5.3 构建系统

| 文件 | 改动 |
| --- | --- |
| `CMakeLists.txt` | Android 下 GPU 默认关闭；warning 选项裁剪 |
| `src/CMakeLists.txt` | `if(ANDROID)` 链接分支 |
| `cmake/CompilerWarnings.cmake` | 移除 Clang/Android 未知 warning |
| `scripts/build_android.sh` | 新增；循环构建 ABI / static / shared / examples |
| `.github/workflows/android.yml` | 新增；NDK 交叉编译门禁 |

### 5.4 测试与示例

| 文件 | 改动 |
| --- | --- |
| `examples/tutorial/12_blocking_io_worker.cpp` | `std::stop_token` 改为 `executor::StopToken` |
| 相关测试 | 相同迁移；`/proc` 和 `sched_getcpu` 测试增加 `__ANDROID__` 守卫 |
| 新增设备测试脚本 | `adb shell` 运行 native smoke test |

---

## 6. 验收标准

一期完成必须同时满足：

1. `cmake` + NDK 在 CI 上构建 arm64-v8a / x86_64，API 21，static + shared，CPU-only，零 error。
2. `EXECUTOR_BUILD_EXAMPLES=ON` 全部 CPU 示例和 tutorial 交叉编译成功。
3. `basic_submit`、`tutorial_12_blocking_io_worker`、至少一个 `comm` 示例在 arm64 真机或模拟器运行通过。
4. `find_package(executor)` 消费者可用，文档写明 `CMAKE_FIND_ROOT_PATH` / `executor_DIR` 要求。
5. Android 上 priority / affinity / mlock / timer slack 失败均不改变任务接受结果，状态字段可观测。
6. ARM 设备完成 MPSC / worker queue 并发压力与批量路径测试，无数据竞争、丢任务或队列状态翻转。
7. 现有 Linux / Windows CI 和 API 不回归；桌面端 `std::stop_token` override 保持源码和 ABI 兼容。

---

## 7. 非目标

- 不实现 Android Service / WorkManager / JNI 业务封装。
- 不实现 Vulkan / NNAPI / OpenCL 计算后端。
- 不承诺任何 Android 设备上的硬实时。
- 不为 OEM 修改过的 bionic 或内核提供逐厂商适配。
- 不在核心库引入 Android Java/Kotlin 依赖。
- 不把 `RealtimeAllocationGuard` 的 Linux `mlockall` 诊断模型推广到 Android。

---

## 8. 风险与待决项

| 风险 | 等级 | 缓解 |
| --- | --- | --- |
| ARM 弱内存序暴露无锁队列缺陷 | 高 | 真机压力 + 批量/取消路径专项；必要时禁用 `EXECUTOR_LOCKFREE_QUEUE` 或回退有锁队列 |
| libc++ 能力随 NDK 版本漂移 | 中 | 双 NDK CI；`executor::StopToken` 用 `__cpp_lib_jthread` 探测 |
| 厂商 bionic / SELinux / cpuset 差异 | 中 | 所有调优 best-effort；设备矩阵测试 |
| 默认线程数过高导致移动端功耗/卡顿 | 中 | Android 默认上限 4；cpuset 自适应 |
| OpenCL 设备碎片化 | 高 | 一期关闭；后续独立设备清单 |
| 交叉编译 `find_package` 易用性差 | 低 | 文档 + Prefab/AAR 集成 |
| 测试无法在 CI 直接运行 Android 二进制 | 中 | 交叉编译 CI + 自托管设备 runner 或模拟器 job |

待决项（2026-09-22 与 android_port_plan 同步回写）：

- [ ] 一期 CI 是否固定 NDK r26c + r28b，还是只保留一个推荐版本。
  （现状：android.yml 保留双版本；长期收敛决策待定）
- [x] Android 默认线程数上限 4 是否作为公开默认，还是仅作为脚本建议。
  （已作为公开默认：docs/API.md 明示"Android 默认上限 4"，executor_manager.cpp 实现）
- [x] 是否在 Android 上禁用 `RealtimeThreadExecutor` 的自动 `SCHED_FIFO` 建议值。
  （已禁用：v0.5.0 起短周期实时线程不再自动申请 SCHED_FIFO，docs/API.md 有说明）
- [ ] ⏸ 门控（二期决策）：OpenCL 是否进入二期路线图，或长期保持关闭。
