---
layout: home
hero:
  name: Kairo
  text: In-process concurrency infrastructure for C++20 applications
  tagline: One facade for ordinary async tasks, low-latency queues, periodic realtime threads, blocking I/O, and optional GPU work. Start from submit_auto().
  actions:
    - theme: brand
      text: Start in ten minutes
      link: /en/quick-start/build
    - theme: alt
      text: What is Kairo?
      link: /en/getting-started/what-is-kairo
features:
  - title: Finish one task first
    details: Start with submit_auto(lambda) and future.get(); you do not need to understand thread pools, GPUs, or real-time scheduling first.
  - title: Choose APIs by scenario
    details: Introduce priority, delay, periodic, batch, dependency, bounded dispatch, worker, or GPU APIs only when timing, capacity, I/O, or data-transfer constraints require them.
  - title: Examples stay verified
    details: Core snippets point to tutorial sources compiled and smoke-tested by the root CMake project.
---

## At a glance

```cpp
auto& executor = kairo::Executor::instance();
auto answer = executor.submit_auto([] { return 42; });
std::cout << answer.get() << '\n';
executor.shutdown();
```

`get()` both retrieves the result and rethrows an exception from the task. See [your first task](/en/quick-start/first-task) for complete code and expected output.

## Scope and boundaries

Kairo is not a coroutine runtime, a distributed messaging system, or a hard realtime OS. It cannot safely force arbitrary running C++ functions to terminate, and `submit_periodic()` is soft periodic work on the ordinary pool, not a dedicated realtime thread. See [what is Kairo?](/en/getting-started/what-is-kairo) for the complete boundary statement, including the synchronization guarantees introduced in 0.4.0.

## Continue from here

- First use: [build and install](/en/quick-start/build), then [run your first task](/en/quick-start/first-task).
- Learn the library boundary: [what is Kairo?](/en/getting-started/what-is-kairo).
- Upgrade existing code: [versions and migration](/en/reference/version-and-migration).
- The complete API signatures, options, and compatibility notes remain in [`docs/API.md`](https://github.com/Linductor-alkaid/kairo/blob/master/docs/API.md).
- Integrating Kairo with AI? Start with the progressive [Kairo integration skill](https://github.com/Linductor-alkaid/kairo/blob/master/docs/skill/executor-integration/SKILL.md); it also explains how to make the skill available from a downstream project.

## Release information

| Item | Current support |
| --- | --- |
| Platform | Linux, Windows; Android CPU-only via NDK |
| Language | C++20 |
| Build system | CMake 3.16+ |
| Version | `v0.5.3` |
| License | [MIT](https://github.com/Linductor-alkaid/kairo/blob/master/LICENSE) |

<div class="version-note">This guide corresponds to `v0.5.3`; later `master` capabilities become stable promises only after their release tag.</div>
