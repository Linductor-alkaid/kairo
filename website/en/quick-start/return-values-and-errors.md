---
title: Return Values and Errors
description: Retrieve task results with future.get and observe task exceptions in the calling thread.
---

# Return Values and Errors

## Goal

Understand that `submit_auto(lambda)` and explicit future-returning APIs report both values and task exceptions through a future.

## Recommended approach

For work that needs a result or confirmation of success, retain the future and call `get()`:

```cpp
auto value = executor.submit_auto([] { return 42; });
std::cout << value.get() << '\n';

auto work = executor.submit_auto([] { /* side effect */ });
work.get(); // It must still observe an exception.
```

An exception thrown by the task does not directly terminate the worker thread. It is rethrown at the matching `get()` call on the calling thread. `submit()` and explicit priority, delay, batch, and dependency APIs retain their own future semantics. The second task in the tutorial example demonstrates this behavior:

```cpp
try {
    static_cast<void>(failed.get());
} catch (const std::exception& error) {
    std::cerr << "task failed: " << error.what() << '\n';
}
```

Do not confuse a task exception with an initialization error. `initialize()` reports setup failures through `ExecutorResult::ok`, `error_code`, and `message`; task failures are observed through a future, failure callback, or failure status.

`DispatchResult::accepted` and `WorkerHandle` are not futures: the first reports bounded queue admission, while the second reports worker lifecycle. Read [Execution Models and Routing Boundaries](/en/guides/execution-models-and-routing) before treating either as completion.

## When not to retain a future

A deliberately fire-and-forget task may omit its future when failures are handled only by status, callback, or logging. It must still have another observable failure path. A long-running service can combine `set_failure_callback()` with status queries; “no call to `get()`” does not mean “no failure occurred.”

## Next step

Read [initialization and shutdown](/en/quick-start/lifecycle) to learn when to call `initialize()` and `shutdown(true)`.
