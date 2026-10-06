# Android A3 验证记录

本文记录阶段 A3 在当前可用环境中完成的设备侧/架构侧验证。验证目标是：
Android 原生 smoke、Blocking I/O 生命周期、`kairo::comm`、MPSC 并发和
ARM64 弱内存序路径。本文只陈述已执行结果，不替代正式发布时的真机矩阵。

## 验证环境

| 环境 | 配置 | 用途 |
| --- | --- | --- |
| Android 官方模拟器 | `system-images;android-30;google_apis;x86_64`，KVM 加速 | 原生 Android smoke / 生命周期 / A2 默认值 |
| qemu-user 8.2 | `qemu-aarch64` + NDK r26c bionic static ARM64 二进制 | ARM64 bionic 指令路径 smoke |
| GitHub Actions ARM64 runner | `ubuntu-24.04-arm`，Neoverse-N2，4 核 | 真实 ARM64 弱内存序并发与长稳 |
| 桌面 Linux x86_64 | 本机 GCC 13 | 基线回归 |

> qemu-user 的 TCG 不保证精确重现 ARM 弱内存序；它只用于验证 bionic/ARM64
> 指令路径和测试逻辑，弱内存序结论以 GitHub ARM64 runner 为准。

## 官方 Android 模拟器结果

构建：

```bash
ANDROID_NDK_HOME=/path/to/ndk-r26c \
  scripts/build_android.sh --abi x86_64 --api 21 --build-static true --build-shared false --build-tests true
```

运行（会自动采集设备型号、API、内核与 CPU 信息）：

```bash
PATH=/path/to/platform-tools:$PATH ANDROID_SERIAL=emulator-5554 \
  scripts/capture_android_device_info.sh --test-dir build-android/x86_64/static/tests --soak-seconds 0
```

结果：6/6 PASS。

| 测试 | 结果 |
| --- | --- |
| `android_smoke`（async + Blocking I/O + channel + 20k MPSC burst） | PASS |
| `test_stop_token_compat` | PASS |
| `test_executor_manager_android_defaults` | PASS |
| `test_android_lockfree_queue_core`（size/stats/batch/cancellation） | PASS |
| `test_multithread_mpsc`（16 producer push_batch 一致性） | PASS |
| `test_lockfree_worker_queue_concurrent_steal`（4000 tasks） | PASS |

`android_smoke` 另以 `EXECUTOR_ANDROID_SOAK_SECONDS=30` 通过。

## qemu-user ARM64 bionic 静态运行结果

NDK r26c 编译六个 standalone 测试为 `-static` ARM64 bionic 二进制，修正
ELF `PT_TLS` `p_align` 后由 `qemu-aarch64` 运行：6/6 PASS。

其中 `test_multithread_mpsc` 初次失败暴露的是测试消费者退出条件在慢速
模拟器下过早退出，不是队列缺陷；修正为“所有 producer 完成后才允许以空
pop 判定耗尽”后通过。该修正已合入 `tests/test_multithread_mpsc.cpp`。

## GitHub Actions ARM64 真实硬件结果

Workflow：`.github/workflows/arm64-concurrency.yml`

Runner 拓扑：

- Architecture：`aarch64`
- CPU：`Neoverse-N2`，4 核，单 socket，NUMA 0-3
- 4 核正常调度：6/6 PASS
- `taskset -c 0` 单核复测 lockfree 三项：PASS
- ASan + UBSan：lockfree 三项 PASS
- 长稳：`EXECUTOR_ANDROID_SOAK_SECONDS=600`，10 分钟 MPSC soak PASS

GitHub Actions run：

| Run | 结论 |
| --- | --- |
| `31925018609`（30s soak，4 核） | success |
| `31925199660`（600s soak，4 核） | success |
| `31925650990`（30s soak + 单核 + sanitizers） | success |
| `31925718238`（补充 runner 拓扑输出） | success |

## 未覆盖项

- **big.LITTLE 真机**：当前无可用 Android 物理设备；Neoverse-N2 是同构 4
  核服务器，不能替代 big.LITTLE 调度器差异验证。
- **Android arm64 官方模拟器**：Google 模拟器拒绝在 x86_64 主机运行
  arm64-v8a AVD；qemu-user 结果不能证明模拟器/真机完整系统行为。
- **TSAN**：ARM64 runner 已跑 ASan/UBSan；TSAN 在该 runner 上的运行时支持
  需单独评估。

正式发布前应在至少一台 big.LITTLE Android 真机执行同一测试集和 10 分钟
soak，并记录 `/proc/cpuinfo`、内核版本和 `getprop ro.build.version.release`。
