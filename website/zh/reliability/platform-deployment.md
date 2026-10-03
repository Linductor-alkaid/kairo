---
title: Linux、Windows 与 Android 部署核对
description: 核对 Linux、Windows 与 Android 的编译产物、CPU 可用范围、实时调度、内存锁定、计时器和运行状态，区分平台预期差异与真实部署失败。
---

# Linux、Windows 与 Android 部署核对

## 先定义部署是否合格

“程序启动成功”只证明进程能运行，不证明请求的线程优先级、CPU 亲和性、内存锁定或计时精度已经生效。部署验收应分成三层：

1. **基础正确性**：普通任务、future 异常、通信和有界关闭行为正确。
2. **平台能力**：目标机器确实提供所需后端、CPU 集合和系统权限。
3. **运行结果**：Kairo 的状态字段证明请求已应用，负载测试证明延迟与 jitter 达标。

如果业务只使用普通线程池，第一层通常已经足够；不要为了“更快”默认申请实时权限。只有控制周期或尾延迟目标明确时，才进入后两层。

## 构建与运行差异

| 项目 | Linux | Windows | Android |
| --- | --- | --- | --- |
| 推荐工具链 | CMake 3.16+，GCC/Clang，C++20 | CMake 3.16+，Visual Studio 2019+ / MSVC，C++20 | NDK r26c/r28b + CMake；一期 CPU-only |
| 线程实现 | pthread | Windows thread API | bionic pthread |
| CMake 生成器 | 通常为单配置 | Visual Studio 通常为多配置，构建和 CTest 要指定 `Release` | NDK toolchain + Ninja/Make；由 `scripts/build_android.sh` 驱动 |
| 实时优先级 | `SCHED_FIFO` 1–99，需要系统授权 | 映射到 `SetThreadPriority` 等级，不等价于 Linux `SCHED_FIFO` | 默认不自动申请 `SCHED_FIFO`；显式配置仍 best-effort，普通 App 通常 `priority_applied=false` |
| CPU 亲和性 | 当前线程允许的 cpuset 内选择 | `SetThreadAffinityMask`；当前实现使用单个 64 位 mask | `sched_setaffinity` 当前允许 cpuset；受 cgroup/SELinux 限制，失败非致命 |
| 内存锁定 | 显式 opt-in 的 `mlockall(MCL_CURRENT \| MCL_FUTURE)`，影响整个进程和后续映射 | 没有等价实现，`process_memory_lock_applied` 预期为 false | bionic 提供 `mlockall`，但普通 App 通常无权限，`process_memory_lock_applied` 预期为 false |
| timer slack | `PR_SET_TIMERSLACK` | 没有 per-thread 等价实现，`timer_slack_applied` 预期为 false | `prctl(PR_SET_TIMERSLACK)` 存在，厂商内核可能忽略，状态字段为准 |
| 短周期计时 | Linux 单调时钟与 timer slack | 周期短于 20ms 时，线程生命周期内请求 1ms timer period | `std::chrono::steady_clock` 软周期；不承诺硬实时精度 |

Linux Release 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DKAIRO_BUILD_TESTS=ON \
  -DKAIRO_BUILD_EXAMPLES=ON \
  -DKAIRO_ENABLE_GPU=OFF
cmake --build build -j
ctest --test-dir build -L tutorial --output-on-failure
```

Android CPU-only Release 构建：

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r26c
scripts/build_android.sh --abi arm64-v8a,x86_64 --api 21 --build-tests true
```

Windows PowerShell Release 构建：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" `
  -DKAIRO_BUILD_TESTS=ON `
  -DKAIRO_BUILD_EXAMPLES=ON `
  -DKAIRO_ENABLE_GPU=OFF
cmake --build build --config Release
ctest --test-dir build -C Release -L tutorial --output-on-failure
```

首次部署先保持 GPU 关闭并运行基础 smoke tests。GPU 驱动、运行时和设备可见性应作为独立能力验收，避免把普通任务故障与 GPU 环境混在一起。

## Linux：部署前权限检查

以下命令只读取当前 shell/进程可见的限制，可直接复制执行：

```bash
uname -srmo
cmake --version
c++ --version
nproc
taskset -pc $$
ulimit -r
ulimit -l
grep -E 'Cpus_allowed_list|Mems_allowed_list' /proc/self/status
```

重点解释：

- `taskset -pc $$` 与 `Cpus_allowed_list` 给出当前进程真正允许使用的 CPU；容器中的 CPU 编号不一定从 0 开始。
- `ulimit -r` 是当前 shell 可申请的实时优先级上限。请求 `SCHED_FIFO` 但没有足够的 `RLIMIT_RTPRIO` 或 `CAP_SYS_NICE` 时，线程仍可运行，但 `priority_applied=false`。
- `ulimit -l` 是可锁内存上限，通常以 KiB 显示。显式请求进程级 `mlockall` 时，上限不足或没有 `CAP_IPC_LOCK` 会导致失败；检查 `process_memory_lock_applied` 和 `process_memory_lock_errno`。
- `Mems_allowed_list` 用于确认容器或 NUMA 策略是否限制了可用内存节点；它不是 Kairo 配置字段，但会影响实际抖动和局部性。

如果程序文件通过 capability 获权，可核对：

```bash
getcap ./your-service
```

不要直接把 `sudo` 运行整个服务当作长期方案。优先由 systemd、容器运行时或安全策略授予最小权限和资源上限，并让部署配置接受安全审查。

### systemd 与容器

systemd 服务至少要核对 `LimitRTPRIO=`、`LimitMEMLOCK=`、`CPUAffinity=` 以及实际运行用户。容器还要同时核对：

- 宿主机给容器的 cpuset；
- 实时调度所需 capability 与 rtprio limit；
- 内存锁定 capability 与 memlock limit；
- 编排系统是否又覆盖了 CPU 和安全策略。

宿主 shell 的 `ulimit` 不能证明容器内相同。必须在**最终运行环境、最终用户身份**下重新执行检查命令。

## Windows：部署前状态检查

在与服务相同的 Windows 主机和账号下执行：

```powershell
[Environment]::OSVersion.VersionString
cmake --version
Get-CimInstance Win32_OperatingSystem |
  Select-Object Caption, Version, OSArchitecture
Get-CimInstance Win32_ComputerSystem |
  Select-Object NumberOfLogicalProcessors, TotalPhysicalMemory
Get-Process -Id $PID |
  Select-Object ProcessName, PriorityClass, ProcessorAffinity
```

这些命令用于记录环境，不代替 Kairo 的线程级状态。Windows 上需要特别注意：

- `SetThreadPriority` 是 Windows 调度提示，与 Linux `SCHED_FIFO` 的保证和数值范围不同；不要复用同一套优先级验收阈值。
- 当前亲和性实现使用一个 64 位 mask。超过 64 个逻辑处理器或涉及 processor groups 的主机，不能假设一个 `cpu_affinity` 列表覆盖全机；必须在目标硬件上验证。
- `process_memory_lock_applied=false` 和 `timer_slack_applied=false` 是当前 Windows 实现的预期结果，不代表注册失败。
- 短周期实时线程会在其生命周期内请求 1ms timer period；这可能增加系统功耗，停止线程后会恢复请求。
- 线程名称依赖 `SetThreadDescription`，目标系统应为 Windows 10 1607 或更高版本；名称只用于诊断，不影响任务正确性。

服务账号、交互式 PowerShell 和 CI runner 可能拥有不同权限与 CPU 限制。不要只在管理员终端验证后就认为 Windows Service 环境等价。

## Android：部署前状态检查

在与最终 APK/AAR 相同的设备或模拟器上执行：

```bash
PATH=/path/to/platform-tools:$PATH ANDROID_SERIAL=<serial> \
  scripts/capture_android_device_info.sh \
  --test-dir build-android/arm64-v8a/static/tests \
  --soak-seconds 600
```

脚本会输出设备型号、Android API、内核、ABI、CPU 信息，并运行 standalone smoke 与
MPSC soak。解释时注意：

- 普通 App 拿不到 `CAP_SYS_NICE` / `CAP_IPC_LOCK`；priority、affinity、memory lock
  为 false 是预期降级，不是注册失败。
- 默认线程池在 Android 上最多 4 个 worker；自动 affinity 只从 cgroup 允许 cpuset 选择。
- 短周期实时线程不会自动申请 `SCHED_FIFO`。确有权限时应显式设置 `thread_priority`，
  再检查 `priority_applied`。
- Android 一期不启用 CUDA/OpenCL；GPU API 返回 `BackendUnavailable`。
- big.LITTLE 真机在发布前为强制验证 gate；仅模拟器或同构 ARM64 runner 结果不能替代。

## 用运行状态确认请求是否生效

平台命令说明“可能具备能力”，Kairo 状态才说明这次运行的申请结果。启动实时任务后，输出一次结构化核对记录：

```cpp
const auto status = executor.get_realtime_executor_status("control-loop");

std::cout
    << "running=" << status.is_running
    << ", period_ns=" << status.cycle_period_ns
    << ", priority=" << status.priority_applied
    << ", affinity=" << status.cpu_affinity_applied
    << ", process_memory_lock=" << status.process_memory_lock_applied
    << ", process_memory_lock_errno=" << status.process_memory_lock_errno
    << ", timer_slack=" << status.timer_slack_applied
    << ", cycles=" << status.cycle_count
    << ", cycle_timeouts=" << status.cycle_timeout_count
    << ", dropped=" << status.dropped_task_count
    << '\n';
```

不要在线程刚启动的同一瞬间只读一次。先等待 `cycle_count` 开始增长，再采集状态；随后在稳定负载和过载负载下分别比较计数增量。

### 按需求定义通过条件

| 需求 | 最小通过条件 |
| --- | --- |
| 只需跨平台后台周期 | `is_running=true`、`cycle_count` 增长、退出在预算内；调优字段可为 false。 |
| Linux 需要固定 CPU | `cpu_affinity_applied=true`，配置 CPU 属于进程允许 cpuset，并通过系统工具复核。 |
| Linux 需要实时调度 | `priority_applied=true`，同时在目标负载下验证尾延迟和 jitter。 |
| Linux 需要防分页抖动 | 显式设定 `enable_process_memory_lock=true`，要求 `process_memory_lock_applied=true`，并验证整个进程内存峰值不会突破部署预算。 |
| Windows 短周期控制 | `is_running=true`、周期统计达标；不要求 Linux 专属的 memory/timer-slack 状态。 |
| Android CPU-only 服务 | 基础 smoke、通信与关闭通过；调优字段 false 时已在业务健康检查中显式接受该降级。 |

调优字段为 false 时，库会安全继续运行并记录 tuning fallback。是否允许继续接流量必须由业务需求决定：后台刷新可以降级，硬性控制预算则应让健康检查失败。

## CPU 亲和性：避免写死错误核心

`RealtimeThreadConfig::cpu_affinity` 为空时，Kairo 会在当前线程允许的 CPU 集合内轮询选核；只有至少两个允许 CPU 时才会自动绑定。显式配置会按原值申请，越过容器 cpuset 或平台范围时申请失败。

推荐部署流程：

1. 先读取最终进程允许的 CPU 集合。
2. 从该集合中选择控制线程核心，并为操作系统、中断和普通 worker 保留容量。
3. 显式配置后检查 `cpu_affinity_applied`。
4. 用系统工具复核实际线程亲和性，并在完整负载下测量，而不是只跑空循环。

不要把开发机的 CPU 编号直接复制到不同 SKU、虚拟机或容器。

## GPU 平台核对

GPU 问题分三层记录，缺一不可：

1. **构建层**：CMake 是否启用 CUDA/OpenCL，对应头文件和库是否存在。
2. **运行层**：驱动、运行时和设备是否对最终服务账号可见。
3. **Kairo 层**：`register_gpu_executor_ex()` 的 `error_code`/`message`，以及注册后的 `GpuExecutorStatus::last_error_message`。

Linux 常用设备工具和 Windows 厂商工具只能证明驱动视角；最终仍要运行一次真实 kernel，并消费 `submit_gpu()` 返回的 future。无 GPU 路径应单独验证 CPU 回退结果正确。Android 一期为 CPU-only，不把设备 GPU 能力纳入验收。

## 部署验收记录

每种正式环境至少保存一份可复现记录：

```text
构建版本 / commit:
操作系统与架构:
编译器与 CMake:
服务账号与启动方式:
允许 CPU / 显式亲和性:
实时优先级限制:
memlock 限制:
GPU 后端 / 驱动 / 设备:
RealtimeExecutorStatus:
基础 smoke tests:
稳定负载结果:
过载与关闭结果:
已接受的调优回退:
```

环境升级、容器基镜像变化、CPU SKU 变化、服务账号或安全策略变化后重新执行，不要沿用旧机器的结论。

## 下一步

遇到具体运行症状时回到[按症状排查运行故障](/zh/reliability/troubleshooting)；实时状态字段和队列语义见[启动专用实时控制循环](/zh/realtime-and-communication/realtime-control)，GPU 注册与降级见[诊断后端并安全降级](/zh/gpu/diagnostics)。
