---
title: Linux, Windows, and Android Deployment
description: Verify Linux, Windows, and Android build artifacts, CPU availability, real-time scheduling, memory locking, timers, and runtime status to distinguish expected platform differences from deployment failure.
---

# Linux, Windows, and Android Deployment

## Define deployment success first

“The program started” proves only that the process can run—not that requested priority, CPU affinity, memory locking, or timer precision applied. Deployment acceptance has three layers:

1. **Basic correctness:** ordinary tasks, future exceptions, communication, and bounded shutdown work.
2. **Platform capability:** target machine offers the needed backend, CPU set, and permission.
3. **Runtime outcome:** Kairo status confirms requests applied, and load testing meets latency/jitter targets.

For an ordinary thread pool, the first layer is usually enough. Do not request real-time privilege by default “for speed”; enter the latter layers only for an explicit control-period or tail-latency target.

## Build and runtime differences

| Item | Linux | Windows | Android |
| --- | --- | --- | --- |
| Toolchain | CMake 3.16+, GCC/Clang, C++20 | CMake 3.16+, Visual Studio 2019+/MSVC, C++20 | NDK r26c/r28b + CMake; CPU-only in this stage |
| Thread implementation | pthread | Windows thread API | bionic pthread |
| CMake generator | Usually single-config | Visual Studio usually multi-config; build/CTest specify `Release` | NDK toolchain with Ninja/Make; driven by `scripts/build_android.sh` |
| Real-time priority | `SCHED_FIFO` 1–99 with authorization | `SetThreadPriority` level; not equivalent to `SCHED_FIFO` | Does not auto-request `SCHED_FIFO`; explicit settings remain best-effort and usually report false |
| CPU affinity | Select within current allowed cpuset | `SetThreadAffinityMask`; current implementation uses one 64-bit mask | `sched_setaffinity` within allowed cpuset; cgroup/SELinux may reject, non-fatally |
| Memory locking | Explicit opt-in `mlockall(MCL_CURRENT | MCL_FUTURE)` for the whole process and future mappings | No equivalent; `process_memory_lock_applied` is expected false | `mlockall` exists but ordinary apps lack permission; `process_memory_lock_applied` expected false |
| Timer slack | `PR_SET_TIMERSLACK` | No per-thread equivalent; `timer_slack_applied` expected false | `prctl(PR_SET_TIMERSLACK)` exists; vendor kernels may ignore it—trust status fields |
| Short-period timing | Monotonic clock and timer slack | Requests 1 ms timer period for thread lifetime when period <20 ms | `steady_clock` soft periods; no hard real-time guarantee |

Linux Release build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DKAIRO_BUILD_TESTS=ON \
  -DKAIRO_BUILD_EXAMPLES=ON \
  -DKAIRO_ENABLE_GPU=OFF
cmake --build build -j
ctest --test-dir build -L tutorial --output-on-failure
```

Android CPU-only Release build:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r26c
scripts/build_android.sh --abi arm64-v8a,x86_64 --api 21 --build-tests true
```

Windows PowerShell Release build:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" `
  -DKAIRO_BUILD_TESTS=ON `
  -DKAIRO_BUILD_EXAMPLES=ON `
  -DKAIRO_ENABLE_GPU=OFF
cmake --build build --config Release
ctest --test-dir build -C Release -L tutorial --output-on-failure
```

Start deployments with GPU disabled and basic smoke tests. Validate GPU driver/runtime/device visibility separately from ordinary-task health.

## Linux: check final-environment permissions

Run under the final shell/process identity:

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

`taskset` and `Cpus_allowed_list` identify CPUs actually available to the process; container numbering may not begin at zero. `ulimit -r` bounds requested real-time priority: insufficient `RLIMIT_RTPRIO`/`CAP_SYS_NICE` leaves the thread running but `priority_applied=false`. `ulimit -l` bounds lockable memory: insufficient value/`CAP_IPC_LOCK` makes an explicitly requested process-wide `mlockall` fail; inspect `process_memory_lock_applied` and `process_memory_lock_errno`. `Mems_allowed_list` is not Kairo configuration but can affect locality and jitter.

If file capabilities grant authority, inspect `getcap ./your-service`. Do not use running the entire service under `sudo` as a long-term fix. Give minimal capability/resource limits through systemd, container runtime, or security policy. Check `LimitRTPRIO=`, `LimitMEMLOCK=`, `CPUAffinity=`, final user, host cpuset, capability/limits, and orchestration overrides in the final container/service—not only a host shell.

## Windows: check the final service account

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

These record environment but do not replace thread-level Kairo state. Windows priority semantics differ from Linux; one 64-bit affinity mask does not cover all processor-group cases on hosts above 64 logical CPUs; false `process_memory_lock_applied` and `timer_slack_applied` are expected in this implementation; short-period threads request 1 ms timer precision (with power cost) during life; thread naming requires Windows 10 1607+ and is diagnostic only. A service account, interactive shell, and CI runner can have different limits.

## Android: check the final device or emulator

Run on the final device or emulator:

```bash
PATH=/path/to/platform-tools:$PATH ANDROID_SERIAL=<serial> \
  scripts/capture_android_device_info.sh \
  --test-dir build-android/arm64-v8a/static/tests \
  --soak-seconds 600
```

The script records model, Android API, kernel, ABI, CPU topology, and standalone tests.
Interpretation notes:

- Ordinary apps lack `CAP_SYS_NICE` / `CAP_IPC_LOCK`; false priority/affinity/memory-lock
  fields are expected fallback, not registration failure.
- The default thread pool is capped at four workers on Android; automatic affinity uses
  only the cgroup-allowed cpuset.
- Short-period realtime threads do not auto-request `SCHED_FIFO`. If the device grants
  permission, set `thread_priority` explicitly and verify `priority_applied`.
- Android in this stage is CPU-only; GPU APIs return `BackendUnavailable`.
- big.LITTLE device validation is a mandatory release gate; emulator or homogeneous
  ARM64 runner results cannot replace it.

## Confirm requests through runtime status

Platform inspection shows possible capability. Kairo status shows this run's result:

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
    << ", dropped=" << status.dropped_task_count << '\n';
```

Wait until `cycle_count` grows, then compare status deltas under steady and overloaded load.

| Requirement | Minimum passing condition |
| --- | --- |
| Cross-platform background period only | `is_running=true`, growing `cycle_count`, exit in budget; tuning fields may be false |
| Linux fixed CPU | `cpu_affinity_applied=true`, configured CPU allowed by process cpuset, verified with system tool |
| Linux real-time scheduling | `priority_applied=true`, plus tail latency/jitter validation under target load |
| Linux paging-jitter mitigation | Explicitly set `enable_process_memory_lock=true`, then require `process_memory_lock_applied=true` and keep the entire process memory peak inside the deployment budget |
| Windows short-period control | Running and period statistics meet target; Linux-specific memory/timer-slack fields not required |
| Android CPU-only service | Basic smoke, communication, and shutdown pass; false tuning fields are explicitly accepted by business health checks |

If tuning falls back, the library runs safely and records it. Business requirements decide whether to keep accepting traffic: background refresh may degrade; hard control budget should fail health checks.

## Affinity, GPU, and deployment record

With empty `RealtimeThreadConfig::cpu_affinity`, Kairo round-robins among CPUs allowed to the current thread and auto-binds only if at least two are allowed. For explicit affinity: read the final allowed set, reserve capacity for OS/interrupts/ordinary workers, verify `cpu_affinity_applied`, inspect actual affinity with system tools, and measure under full load. Never copy development-machine CPU numbering into another SKU, VM, or container.

For GPU, record three layers: CMake CUDA/OpenCL enablement and headers/libraries; driver/runtime/device visibility to the final account; then `register_gpu_executor()` result and post-registration `GpuExecutorStatus::last_error_message`. A real kernel future is still required; validate CPU fallback independently. Android in this stage is CPU-only and does not accept GPU capabilities.

Save build version/commit, OS/architecture, compiler/CMake, service identity/start method, allowed/explicit CPUs, real-time and memlock limits, GPU backend/driver/device, realtime status, smoke tests, steady/overload/shutdown results, and accepted tuning fallbacks. Re-run after base-image, CPU SKU, service-account, or security-policy changes.

For a live symptom return to [troubleshoot by symptom](/en/reliability/troubleshooting); see [dedicated real-time control](/en/realtime-and-communication/realtime-control) for status/queue semantics.
