---
title: Initialization and Shutdown
description: Configure Kairo before first use and stop it deliberately at an application boundary.
---

# Initialization and Shutdown

The minimal program can rely on lazy initialization. When you need custom thread counts, queue capacity, or monitoring, fix configuration before the first submission.

```cpp
kairo::ExecutorConfig config;
config.min_threads = 2;
config.max_threads = 4;

auto& executor = kairo::Executor::instance();
auto initialized = executor.initialize_ex(config);
if (!initialized) {
    throw std::runtime_error(initialized.message);
}

// submit_auto(lambda) ... future.get() ...
executor.shutdown(true);
```

`initialize_ex()` returns an `ExecutorResult` with an error code and message, which is more useful for diagnosis than the compatible `bool` initializer. `shutdown(true)` waits for accepted asynchronous work; when the library's default wait limit is exceeded, it records a timeout diagnostic and continues with non-waiting shutdown. It is not an infinite-wait guarantee.

The singleton has a `shutdown(false)` process-exit fallback, and independent `Executor` instances clean up through destruction. Neither removes the application's responsibility to decide whether accepted work must finish at its business boundary.

For a low-frequency health check or shutdown record, `get_snapshot()` returns the
current lifecycle plus registered backend states, failure summary, and aggregate
counters. It is read-only and does not trigger lazy initialization, so an
uninitialized instance remains `Created` after the query. Treat the result as a
best-effort diagnostic snapshot rather than a submission reservation or a global
transactional read.

## Common mistakes

- Changing initialization configuration after the first `submit_auto()` or `submit()` completed lazy initialization.
- Exiting without waiting for owned futures or choosing a shutdown policy.
- Treating initialization errors and task errors as the same signal: inspect `ExecutorResult` for setup and futures for task execution.

For version-specific compatibility and upgrade guidance, read [versions and migration](/en/reference/version-and-migration).
