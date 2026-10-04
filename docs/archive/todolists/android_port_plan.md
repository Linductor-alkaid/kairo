# Android 适配实施计划

本文档把 [Android 适配方案](../design/android_port.md) 拆分为可提交、可验证的实现任务。
目标是先交付 **CPU-only、best-effort 调度、可交叉编译、可在真机 smoke test** 的 executor Android 支持；
GPU、OpenCL、硬实时和弱内存序性能调优不进入一期完成定义。

所有 [ ] 均为待办；合入顺序遵循“先构建系统与平台层，再公开 API 兼容层，最后设备验证与分发”。

---

## 当前基线

- [x] 核心库为 C++20 / CMake 3.16+，无第三方必需依赖。
- [x] 桌面平台已有 Linux + Windows 实现与 CI。
- [x] `ThreadPoolExecutor`、`RealtimeThreadExecutor`、`BlockingIoExecutor`、`ExecutorManager`、`executor::comm` 和监控面均已存在。
- [x] 已用 NDK r26c / r28b 完成可行性交叉编译实验。
- [x] 当前 `src/executor/util/thread_utils.cpp` 使用 `pthread_setaffinity_np` / `pthread_getaffinity_np`，Android 无法编译。
  （已过时基线：一期已加 `__ANDROID__` 分支改用 `sched_setaffinity(gettid(),...)`，2026-09-22 回填）
- [x] 当前 `include/executor/blocking_io.hpp` 暴露 `std::stop_token`，NDK r26c 无此头文件。
  （已过时基线：已迁移至 `executor::StopToken` 兼容层，2026-09-22 回填）
- [x] 当前 `src/CMakeLists.txt` 在 Android 上链接不存在的 `librt` 并导出 `atomic`。
  （已过时基线：`if(ANDROID)` 分支已不链 rt、不导出 atomic，2026-09-22 回填）
- [x] 当前无 Android CI、打包脚本、Gradle/Prefab 集成和设备测试路径。
  （已过时基线：android.yml、build_android.sh、PACKAGE_ANDROID.md、Prefab 均已落地，2026-09-22 回填）

---

## 实施原则与不可变约束

1. **公开 API 桌面兼容**：`executor::StopToken` 在桌面必须是 `std::stop_token` 别名，不破坏现有 override 和 ABI。
2. **不依赖实验性标准库**：Android 不得要求消费者手动开启 `-fexperimental-library`。
3. **平台调优 best-effort**：Android 上 priority / affinity / mlock / timer slack 失败不改变任务接受结果，必须可通过状态字段观察。
4. **编译通过不等于并发正确**：ARM 弱内存序路径必须通过设备侧压力与功能测试。
5. **一期 GPU 关闭**：CUDA/OpenCL 默认不在 Android 构建中启用；API 保持 `BackendUnavailable` 语义。
6. **生命周期显式化**：JNI/App 集成文档必须要求显式 `shutdown()`，不依赖静态析构。
7. **桌面不回归**：每个阶段合入后，Linux / Windows 现有测试必须保持通过。
8. **文档以可复现命令为准**：所有 Android 命令必须在文档中给出，且 CI 与文档使用同一脚本。

---

## 阶段 A0：Android 构建系统与平台层

> 进度说明：A0 已随 A1 的 `executor::StopToken` 兼容层一起完成代码任务；
> CI workflow 已新增，默认 Android 构建不再依赖 `-fexperimental-library`。

### 任务

- [x] 在 `src/CMakeLists.txt` 增加 `if(ANDROID)` 分支：
  - [x] `find_package(Threads REQUIRED)` 保留。
  - [x] 不链接 `rt`。
  - [x] 不把 `atomic` 作为 Android 公共依赖导出。
- [x] 在根 `CMakeLists.txt` 中，Android 构建默认关闭 GPU：
  - [x] `EXECUTOR_ENABLE_CUDA` 默认 OFF。
  - [x] `EXECUTOR_ENABLE_OPENCL` 保持现有默认 OFF。
  - [x] 保留用户显式覆盖能力。
- [x] 在 `src/executor/util/thread_utils.cpp` 增加 `__ANDROID__` affinity 路径：
  - [x] `set_cpu_affinity()` 使用 `sched_setaffinity(gettid(), ...)`。
  - [x] `get_current_thread_affinity()` 使用 `sched_getaffinity(gettid(), ...)`。
  - [x] 保持返回值语义：空列表 / false 表示未生效。
- [x] 处理 NDK clang 不支持的 warning 选项：
  - [x] `-Wlogical-op`、`-Wnoexcept`、`-Wstrict-null-sentinel` 不用于 Android/Clang。
  - [x] 保持 GCC 与 MSVC 现有 warning 行为不变。
- [x] 新增 `scripts/build_android.sh`：
  - [x] 参数：NDK 路径、ABI、API level、static/shared、examples 开关。
  - [x] 默认 arm64-v8a + x86_64，API 21，CPU-only。
  - [x] 输出物统一到 `build-android/<abi>/...`。
- [x] 新增 `.github/workflows/android.yml`：
  - [x] NDK r26c 与 r28b 双版本交叉编译。
  - [x] arm64-v8a / x86_64，static + shared。
  - [x] 缓存 NDK，不上传 GB 级 SDK 到 artifacts。
  - [x] 当前只做构建门禁，不运行设备测试。

### 验收

- [x] 默认配置（无 `-fexperimental-library`）在 NDK r26c / r28b 上产出 CPU-only 静态库与共享库。
- [x] arm64-v8a / x86_64，API 21，static + shared 已本地交叉编译验证。
- [x] `basic_submit` 和全部 CPU examples/tutorial 已交叉编译通过。
- [x] Linux 本机构建通过；Windows CI 待远端 workflow 运行后确认，本轮改动未触及 MSVC 分支。
- [x] Android 共享库 `llvm-readelf -d` 不含对 `librt` 的依赖。

### 合并粒度

- [x] CMake 链接分支 + GPU 默认值：独立提交。
- [x] thread_utils Android affinity：独立提交。
- [x] warning 裁剪：独立提交。
- [x] 构建脚本 + CI workflow：已提交。

---

## 阶段 A1：`executor::StopToken` 兼容层与 Blocking I/O 生命周期

### 任务

- [x] 新增 `include/executor/stop_token.hpp`：
  - [x] 桌面：`using StopToken = std::stop_token;`。
  - [x] Android 且 `__cpp_lib_jthread` 不可用时启用自有实现。
  - [x] 提供 `StopToken::stop_requested()`。
  - [x] 提供 `StopSource::request_stop()` / `get_token()`。
  - [x] 提供 `detail::JThread`：可移动、析构 request_stop + join。
  - [x] 不在 `std` 命名空间注入符号。
  - [x] 头文件可独立包含。
- [x] `include/executor/blocking_io.hpp`：
  - [x] `IBlockingIoWorker::run()` 参数改为 `executor::StopToken`。
  - [x] 文档注释继续说明 stop token 只表达请求，wakeup 才能解除底层阻塞。
- [x] `src/executor/blocking_io_executor.hpp/.cpp`：
  - [x] 桌面继续使用 `std::jthread`（通过 `detail::JThread` 别名）。
  - [x] Android fallback 使用 `executor::detail::JThread` 与 `StopSource`。
  - [x] `request_stop_locked()` 的平台分支保持“标记 -> request_stop -> wakeup”顺序。
  - [x] `stop()` 的 join 与自停止语义不改变。
- [x] 项目内示例与测试迁移：
  - [x] `examples/tutorial/12_blocking_io_worker.cpp` 使用 `executor::StopToken`。
  - [x] `tests/test_blocking_io_executor.cpp` 与 facade 测试使用 `executor::StopToken`。
  - [x] 确保桌面代码使用 `std::stop_token` 仍可 override（alias 兼容性测试）。
- [x] 新增编译期测试：
  - [x] 桌面 `static_assert(std::is_same_v<executor::StopToken, std::stop_token>)`。
  - [x] Android fallback 的复制/移动和 request_stop 单测（`tests/test_stop_token_compat.cpp`；主机 forced fallback 已运行，真机执行归 A3）。
- [x] 文档：
  - [x] `docs/API.md` 增加 `executor::StopToken` 说明。
  - [x] `docs/MIGRATION.md` 增加 Android Blocking I/O worker 接入说明。

### 验收

- [x] NDK r26c 与 r28b（不传 `-fexperimental-library`）均能编译 `blocking_io.hpp` 并构建完整库。
- [x] `tutorial_12_blocking_io_worker` 交叉编译通过。
- [x] 桌面 Blocking I/O 测试全部通过，ABI/API 不回归。
- [x] 重复 stop、ready 超时、worker 异常、自停止路径在桌面继续通过。
- [x] Android fallback 的 `JThread` 析构/移动不产生 double join（`EXECUTOR_STOP_TOKEN_FORCE_FALLBACK` 已在本机运行时验证；真机执行仍归 A3）。

### 合并粒度

- [x] 公开兼容层头文件：独立提交。
- [x] BlockingIoExecutor 实现迁移：独立提交。
- [x] 示例/测试迁移：独立提交。
- [x] API/MIGRATION 文档：独立提交。

---

## 阶段 A2：Android 运行时语义调整

### 任务

- [x] `ExecutorManager` 默认线程数：
  - [x] Android 下默认 `max_threads` 上限设为 4。
  - [x] `hardware_concurrency()` 失败时继续走现有安全默认。
- [x] 自动 CPU affinity：
  - [x] Android 下不使用 `0..hw-1` 伪列表。
  - [x] 优先从 `util::get_current_thread_affinity()` 获得允许 cpuset。
  - [x] 空 mask 时保持 OS 自由调度。
- [x] 实时线程执行器：
  - [x] 保持 `priority_applied` / `cpu_affinity_applied` / `process_memory_lock_applied` / `timer_slack_applied` 状态语义。
  - [x] 确认 `SCHED_FIFO` EPERM 时只记录失败，不影响周期任务。
  - [x] 决策：Android 默认不自动申请 `SCHED_FIFO`；显式 `thread_priority` 仍 best-effort 尝试。
- [x] 测试代码平台守卫：
  - [x] `/proc/self/task/.../comm` 测试使用 `__linux__ && !defined(__ANDROID__)`。
  - [x] `sched_getcpu()` 测试增加 Android 分支注释，确认 API 21 行为。
  - [x] 仅适合 desktop Linux 的 `/proc` 测试增加 Android 守卫。
- [x] 更新状态字段注释：
  - [x] Android 上 priority / affinity / mlock / timer slack 均标注 best-effort。

### 验收

- [x] Android 默认 async executor 初始化成功且线程数不超过 4：已通过 `-D__ANDROID__` 本机模拟路径运行验证，另已交叉编译。
- [x] 显式 affinity 失败时任务提交和 shutdown 不异常：`test_thread_pool_invalid_cpu_affinity_is_nonfatal` 在桌面与 Android 模拟路径均通过。
- [x] 相关桌面测试通过：`test_thread_pool`、`test_realtime_hardening` 通过。
- [x] 现有 Linux 行为（线程数、affinity 列表）不变：桌面 `test_thread_pool` 全量通过。

### 合并粒度

- [x] 线程数上限：独立提交。
- [x] cpuset 自适应：独立提交。
- [x] 测试守卫：独立提交。

---

## 阶段 A3：设备侧验证与 ARM 弱内存序

### 任务

- [x] 增加 native smoke test 目标或复用现有 examples：
  - [x] `basic_submit`：线程池 submit / future / shutdown。
  - [x] `tutorial_12_blocking_io_worker`：注册、启动、request_stop、join。
  - [x] 至少一个 `comm` 示例：channel/mailbox/phase gate。
- [x] 建立设备测试脚本：
  - [x] `adb push` 测试二进制。
  - [x] `adb shell` 运行并收集退出码与输出。
  - [x] 支持 `ANDROID_SERIAL` 指定设备。
- [x] 获取可在 Android 运行的 GTest：
  - [x] 一期不引入 GTest，采用轻量 assert main 覆盖核心 smoke 与并发路径。
- [x] ARM 并发专项：
  - [x] `test_lockfree_queue_size` / `test_lockfree_queue_status` 核心语义由 standalone `test_android_lockfree_queue_core` 覆盖。
  - [x] `test_lockfree_worker_queue_concurrent_steal`。
  - [x] `test_multithread_mpsc`。
  - [x] batch push/pop 与 reservation cancellation 路径。
  - [x] 单核 pinned、4 核 Neoverse-N2 各一轮。
  - [x] big.LITTLE 真机：当前无可用硬件，已作为发布前 gate 移入 `docs/RELEASE_CHECKLIST.md`；A3 以官方模拟器 + ARM64 Neoverse-N2 4 核/单核关闭。
- [x] 长稳测试：
  - [x] MPSC 多生产者 + 单消费者 soak ≥ 10 分钟（ARM64 runner 600s PASS）。
  - [x] 监控 dropped / queue_full / peak size 无异常翻转。
- [x] 收集结果：
  - [x] 设备型号、Android API、内核版本、ABI：见 `docs/performance/android_a3_validation.md`。
  - [x] 单轮吞吐与失败计数：见 `test_multithread_mpsc` 输出。
  - [x] 如失败，先关闭 `EXECUTOR_LOCKFREE_QUEUE` 复测以定位队列实现问题：本轮无失败，无需降级。

### 验收

- [x] 官方 Android 模拟器 smoke test 全部通过；另有 qemu-user ARM64 bionic 6/6 PASS。
- [x] ARM 并发专项无失败；ARM64 runner 4 核、单核、ASan/UBSan 均 PASS。TSAN 未运行。
- [x] 长稳 MPSC 600s 无队列状态破坏。
- [x] 本轮未发现弱内存序缺陷，无需单独缺陷 PR 或关闭 lockfree 选项。
- [x] big.LITTLE 真机验证：已显式登记为发布前 gate，不作为 A3 完成阻塞；见 `docs/RELEASE_CHECKLIST.md`。

### 合并粒度

- [x] 测试脚本与 smoke 目标：独立提交。
- [x] 设备测试文档：独立提交。
- [x] 并发缺陷修复：按问题独立提交。

---

## 阶段 A4：Android 分发与集成文档

### 任务

- [x] 新增 `docs/PACKAGE_ANDROID.md`：
  - [x] NDK 路径、ABI、API level、STL 选择。
  - [x] 静态库 / 共享库差异。
  - [x] `CMAKE_FIND_ROOT_PATH` / `executor_DIR` 交叉查找说明。
  - [x] AGP `externalNativeBuild` + CMake 接入示例。
  - [x] `c++_shared` 时打包 `libc++_shared.so`。
  - [x] JNI 生命周期中显式 `shutdown()` 示例。
- [x] 更新 `docs/BUILD.md`：
  - [x] 增加 Android 一节与 `scripts/build_android.sh` 入口。
- [x] 更新 README / README_zh 平台表述：
  - [x] badge 与支持平台已更新为 Linux / Windows / Android。
  - [x] 明确 Android 为 CPU-only、调度 best-effort，不宣称硬实时或 GPU。
- [x] 评估 Prefab/AAR：
  - [x] 提供 `packaging/prefab/executor/module.json` 模板。
  - [x] 产出物定义：`libexecutor.so` + 公开头 + prefab metadata。
  - [x] 文档明确不把测试或 examples 打入 AAR。
- [x] 更新 `docs/RELEASE_CHECKLIST.md`：
  - [x] 增加 Android 构建产物、模拟器测试、ARM64 concurrency 与 big.LITTLE 真机 gate。

### 验收

- [x] 最小 AGP/CMake 消费者路径已文档化；`find_package(executor)` 消费者已用 NDK r26c 实际配置并链接通过。
- [x] `CMAKE_FIND_ROOT_PATH` / `executor_DIR` 说明与实测命令一致。
- [x] 共享库模式 `libc++_shared.so` 打包命令已文档化；当前 static/c++_static 消费者实测无额外 libc++ 依赖。
- [x] 文档命令已在本地执行：`build_android.sh`、模拟器 `capture_android_device_info.sh`、`find_package` 消费者均通过。

### 合并粒度

- [x] PACKAGE_ANDROID 文档：独立提交。
- [x] BUILD/README 交叉更新：独立提交。
- [x] Prefab/AAR 支持：独立提交。

---

## 阶段 A5（可选，二期）：OpenCL 后端评估

> 2026-09-22 定性：⏸ 门控（二期决策 + 真机）——依赖 OpenCL 设备与路线图决策，
> `opencl_loader.cpp` 尚无 Android 搜索路径，一期明确不进。

### 任务

- [ ] 调研目标设备 OpenCL 可用性：`/vendor/lib64/libOpenCL.so`、`/system/vendor/lib64`。
- [ ] 在 `opencl_loader.cpp` 增加 Android 搜索路径。
- [ ] 处理 App 对 vendor 库的 `dlopen` namespace / SELinux 限制。
- [ ] 建立设备白名单与运行时探测报告。
- [ ] 在未检测到 OpenCL 时保持 `BackendUnavailable` 语义。

### 验收

- [ ] 至少一个白名单设备完成 OpenCL kernel 提交 smoke test。
- [ ] 无 OpenCL 设备行为与一期相同。
- [ ] 文档明确 Android OpenCL 是可选能力，不是默认能力。

---

## 文档与维护

- [x] 本计划完成后，将完成状态回写到 [Android 适配方案](../design/android_port.md) 的待决项。
  （2026-09-22 已同步：线程上限 4 已成公开默认、SCHED_FIFO 已默认关闭、NDK 现状双版本）
- [x] 在 [项目任务清单](todolist.md) 增加 Android 阶段并链接本计划。
- [x] 所有 Android 相关公开文档必须有对应可执行命令或已运行记录。
  （A4 验收已确认文档命令均本地执行过，2026-09-22 回填）
- [x] 涉及公开 API 的文档不得早于实现合并。
  （一次性过程约束，0.5.0 已按此交付，2026-09-22 回填）

---

## 建议合并顺序

1. A0：平台构建基础，先让 CI 能交叉编译失败变少。
2. A1：公开 API 兼容层与 Blocking I/O，解除标准库阻塞。
3. A2：运行时语义，处理 Android 默认值。
4. A3：设备验证，ARM 弱内存序专项。
5. A4：分发与集成文档，最后更新对外平台宣称。
6. A5：可选 OpenCL，独立评审。

每个阶段必须以独立可回滚提交合入；A4 不得早于 A3 的 smoke test 结果，README 平台宣称不得早于 A3 完成。

---

## 风险与待决项

- [ ] NDK 双版本 CI 是否长期保留，或只保留推荐版本。
  （现状：android.yml 保留 r26c + r28b 双版本；长期收敛决策待定，2026-09-22 注记）
- [x] Android 默认线程数上限 4 是否写入公开默认。
  （已写入：docs/API.md "Android 默认上限 4"，executor_manager.cpp 已实现，2026-09-22 回填）
- [x] Android 上自动 `SCHED_FIFO` 建议值是否关闭。
  （已关闭：0.5.0 起短周期实时线程不再自动申请 SCHED_FIFO，docs/API.md 有说明，2026-09-22 回填）
- [x] 无锁队列弱内存序缺陷的降级策略：修复 / 默认关闭 / 要求用户显式启用。
  （已收敛：A3 验收未发现弱内存序缺陷，无需关闭 lockfree 选项；预案留存于
  android_port.md 风险表，2026-09-22 回填）
- [ ] ⏸ 门控（二期决策）：OpenCL 是否进入版本路线图。
- [x] 是否需要自托管 Android runner；如无设备，至少保留 API 21/24 x86_64 模拟器 job。
  （已由替代方案覆盖：手动触发的 arm64-concurrency.yml（ubuntu-24.04-arm，soak 可配），
  设备验证经 A3 本地模拟器完成，2026-09-22 回填）

---

## 完成定义（Definition of Done）

一期 Android 支持只有在以下条件全部满足后才可对外宣布：

- [x] A0、A1、A2、A3、A4 全部验收项通过。
- [x] Linux / Windows 既有 CI 不回归。
- [x] Android CI 稳定构建 arm64-v8a + x86_64，static + shared，API 21。
- [ ] ⏸ 门控（硬件）：至少一台 arm64 设备完成 smoke、Blocking I/O、MPSC 压力与长稳测试。
  （big.LITTLE 真机未到位；模拟器 6/6 与 ARM64 Neoverse-N2 runner 600s soak 已覆盖，
  发布前 gate 见 docs/RELEASE_CHECKLIST.md）
- [x] 打包与集成文档可复现。
- [x] 公开 README / BUILD / API 文档与实际行为一致。
