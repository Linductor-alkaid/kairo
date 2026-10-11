# Kairo

<p align="center">
  <img src="docs/kairo.png" width="160" alt="Kairo logo">
</p>

[![CI](https://github.com/Linductor-alkaid/kairo/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/Linductor-alkaid/kairo/actions/workflows/c-cpp.yml) [![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](https://isocpp.org/) [![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)](https://cmake.org/) [![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE) [![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20Android-1793D1)](https://github.com)

> 中文版：[README_zh.md](README_zh.md)

📖 **Online user guide: [linductor-alkaid.github.io/kairo](https://linductor-alkaid.github.io/kairo/)**

**Kairo is an in-process concurrency infrastructure library for C++20 applications.**

Its unified facade manages ordinary asynchronous tasks, low-latency queues, periodic realtime threads, long-lived blocking I/O, and optional GPU work. It also provides bounded communication, task orchestration, backpressure, and lifecycle diagnostics.

Most users can begin with `submit_auto()`. Move to a specialized path only when the application has explicit timing, capacity, I/O, or data-transfer constraints.

## Why Kairo

- **One entry point, multiple execution models**: a single `Executor` manages thread pools, lock-free low-latency paths, dedicated realtime threads, blocking I/O, and GPU executors.
- **Honest result semantics**: ordinary tasks return futures, bounded dispatch reports admission, and long-lived workers return lifecycle handles. Different models are not disguised behind one completion contract.
- **Common concurrency tools included**: priority, delay, soft periodic work, batching, dependencies, cooperative cancellation, timer handles, a FIFO serial execution context, FIFO channels, latest values, snapshots, phase coordination, and topic fan-out.
- **Observable failure and overload**: submission rejection, task exceptions, timeouts, realtime drops, queue depth, total-admission rejections, and communication latency remain visible through results, status, or callbacks.
- **Progressive adoption**: the default CPU path does not require knowledge of internal executors; realtime, lock-free, and GPU capabilities can be introduced only when needed.

Typical uses include background computation, robotics and device control, sensor acquisition, long-lived I/O services, and local applications with mixed CPU/GPU work.

## Five-Minute Start

### Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Kairo requires C++20 and CMake 3.16+. Linux and Windows are supported. Android CPU-only support is available through the NDK toolchain; see [PACKAGE_ANDROID.md](docs/PACKAGE_ANDROID.md). GPU backends are optional and do not affect the ordinary CPU path when no GPU is available.

### Submit your first task

```cpp
#include <kairo/executor.hpp>
#include <iostream>

int main() {
    auto& ex = kairo::Executor::instance();
    auto result = ex.submit_auto([] { return 42; });

    std::cout << result.get() << '\n';
    ex.shutdown();
}
```

`submit_auto(lambda)` uses the ordinary asynchronous executor by default. `future::get()` returns the result and rethrows task exceptions. Call `initialize()` explicitly only when you need custom thread counts, capacities, or monitoring settings.

See [Your first task](website/en/quick-start/first-task.md) for the complete build, link, and error-handling path.

## Choose by Work Semantics

First decide whether the caller needs to observe task completion, queue admission, or the lifecycle of a long-lived worker.

| Work type | Recommended entry | What the caller receives |
| --- | --- | --- |
| Ordinary finite CPU work | `submit_auto(lambda)` | value or exception in a `future` |
| Priority, delay, soft periodic, batch, or dependencies | corresponding explicit facade API | `future`, task ID, or `TaskHandle` |
| Validated low-latency or periodic realtime path | `dispatch_auto(...)` | bounded queue admission, not completion |
| Long-lived, interruptible I/O loop | `start_worker(...)` | startup result and `WorkerHandle` lifecycle |
| Separate CPU and GPU implementations | `submit_auto(cpu_gpu_task(...))` | selected path completion or exception |
| Data shared between long-running threads | `kairo::comm` | FIFO, latest-value, snapshot, phase, or subscription semantics |

`Auto` does not silently select lock-free or realtime backends for performance. See [Choosing a submission API](website/en/guides/choosing-submit-api.md) for detailed guidance.

> **Batch performance**: `submit_batch()` and `submit_batch_no_future()` can reduce repeated submission overhead, but Kairo does not promise a fixed speedup. The benchmark date is 2026-07-09; results and environment metadata are recorded in [batch_submit_baseline_2026-07-09.json](docs/performance/batch_submit_baseline_2026-07-09.json). Build with `cmake --build build --target benchmark_batch_scales benchmark_batch_submit_real benchmark_batch_submit_concurrent -j2`, then run `./build/tests/benchmark_batch_scales`, `./build/tests/benchmark_batch_submit_real`, and `./build/tests/benchmark_batch_submit_concurrent` to reproduce the measurements.

## Cross-Thread Communication

`kairo::comm` provides in-process components selected by data semantics:

| Requirement | Component |
| --- | --- |
| One consumer processes every message in FIFO order | `MpscChannel<T>` |
| A control cycle drains a bounded number of commands | `RealtimeChannel<T>` |
| Only the newest configuration or target matters | `LatestMailbox<T>` |
| Multiple readers need one complete, consistent state | `DoubleBuffer<T>` |
| Initialization, calibration, and operation advance by phase | `PhaseGate` / `Sequencer` |
| Independent modules receive the same event stream | `Topic<T>` / `TopicSubscription<T>` |

Capacity, drop policy, and close behavior are part of the application contract, not implementation details. See [Choosing a communication component](website/en/guides/choosing-communication.md) for selection and realtime boundaries. The [robot pipeline example](examples/comm_robot_pipeline.cpp) combines these components in one scenario.

## Scope and Boundaries

Kairo deliberately keeps the following boundaries:

- It is not a coroutine runtime and does not provide a coroutine scheduler.
- It is not a distributed messaging system or dataflow framework. Topics provide in-process fan-out, not networking, persistence, replay, or acknowledgement.
- It is not a hard realtime operating system. End-to-end jitter still depends on task bodies, the OS, privileges, CPU isolation, resident memory, and target hardware.
- It cannot safely force arbitrary running C++ functions to terminate. Cancellation (`request_task_cancel` / `StopToken`) is a cooperative request, not preemption; long-lived work must poll its stop token. `TaskOptions::deadline` remains advisory routing/diagnostics and never triggers cancellation by itself.
- `submit_periodic()` is soft periodic work on the ordinary thread pool, not a dedicated realtime thread.
- Default async submission is unbounded by default; configure `max_in_flight_tasks` explicitly when structured overload rejection is required (covers facade submits including serial dispatch, but not timer firing or realtime/GPU — see `docs/API.md` §3.10). `queue_capacity` only sizes per-worker local queues and is not a backpressure bound.
- Facade timer handles (`TimerHandle`) dispatch expiry work to the ordinary thread pool. They do not bind to external event loops (asio strands); timers that must execute and be destroyed on one strand stay application-managed. See the [external event loop interop guide](docs/external_event_loop_interop.md).

Since 0.4.0, key communication synchronization paths use fixed storage and atomic implementations. “Synchronization lock-free” does not cover payload operations, callbacks, page faults, or OS scheduling. `Topic<T>` belongs to the ordinary control plane and is not a realtime primitive. See the [migration notes](docs/MIGRATION.md) for exact guarantees.

## Install and Integrate

```bash
cmake --install build --prefix /usr/local
```

In a consumer project:

```cmake
find_package(kairo REQUIRED)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE kairo::executor)
```

You can also integrate the source tree with `add_subdirectory(path/to/kairo)`. See [BUILD.md](docs/BUILD.md) for static and shared libraries, build options, and release packages.

## Continue Reading

| Goal | Documentation |
| --- | --- |
| Decide whether Executor fits your project | [What is Executor?](website/en/getting-started/what-is-kairo.md) |
| Go from build to a real first task | [Quick start](website/en/quick-start/build.md) |
| Understand primary types and full contracts | [API reference](docs/API.md) |
| Add realtime threads and communication | [Realtime and communication](website/en/realtime-and-communication/index.md) |
| Register and diagnose GPU backends | [GPU execution](website/en/gpu/index.md) |
| Upgrade from an earlier release | [Migration guide](docs/MIGRATION.md) |
| Review release changes | [CHANGELOG](CHANGELOG.md) |

More runnable code is available in [examples](examples/) and [tutorial](examples/tutorial/). For AI-assisted integration, ask the agent to read the [Kairo integration skill](docs/skill/kairo-integration/SKILL.md) first.

## Version and License

Current version: **v0.7.0**

Kairo is available under the [MIT License](LICENSE).
