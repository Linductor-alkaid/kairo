# Advanced Extensions

## Use It For

An externally driven realtime cycle, a custom executor implementation, or direct `ExecutorManager` / backend access that the Facade cannot express. Prefer the Facade until one of those requirements is explicit.

## Custom Cycle Source

Implement `ICycleManager` only when an external clock or scheduler must drive the control cycle. It must implement `register_cycle`, `start_cycle`, `stop_cycle`, and `get_statistics`; give its instance a lifetime longer than `stop_realtime_task()`.

```cpp
MyCycleManager clock;
executor::RealtimeThreadConfig config;
config.thread_name = "control";
config.cycle_period_ns = 1'000'000;
config.cycle_manager = &clock;
config.cycle_callback = [] { control_step(); };
```

Register/start it through the ordinary realtime Facade path after configuration.

## Direct Backends And Manager

`ExecutorManager`, `IAsyncExecutor`, `IRealtimeExecutor`, `IGpuExecutor`, and `IBlockingIoExecutor` exist for custom composition. Their raw getter APIs are non-owning; never retain them across or concurrently with shutdown. Use a manager snapshot API where a lifecycle-holding `shared_ptr` is required.

## Integration Pitfalls

- The application owns a custom `ICycleManager`; Executor only borrows it.
- `stop_cycle()` may synchronously invoke the registered callback, so stop operations must be reentrancy-safe and bounded.
- A manager snapshot keeps an object alive but does not keep it running or prevent shutdown from requesting stop.
- Treat custom executors as an integration commitment: define startup, rejection, error, stop, and status semantics before registering them.

## Related Guide

`website/en/advanced/custom-cycle-manager.md`, `website/en/advanced/escape-hatches.md`, and `docs/API.md`.
