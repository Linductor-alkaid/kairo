---
title: Custom Cycle Source
description: Use ICycleManager to connect an external clock or periodic framework to a realtime task and assume full lifecycle responsibility.
---

# Custom Cycle Source

## Goal

Implement `ICycleManager` and inject it into `RealtimeThreadConfig` only when built-in `sleep_until` timing cannot meet an external clock, hardware trigger, or existing control framework.

## Interface contract

`ICycleManager` has four responsibilities: `register_cycle(name, period_ns, callback)` stores the definition; `start_cycle(name)` runs it; `stop_cycle(name)` requests stop; `get_statistics(name)` reports statistics. Kairo does not own the object. It must stay valid for realtime registration, execution, and stopping. `start_cycle()` normally runs synchronously in the realtime thread start path, so it returns only after `stop_cycle()` and must not accidentally block the caller's shutdown path.

```cpp
class ExternalClock final : public kairo::ICycleManager {
public:
    bool register_cycle(const std::string& name, int64_t period_ns,
                        std::function<void()> callback) override;
    bool start_cycle(const std::string& name) override;
    void stop_cycle(const std::string& name) override;
    kairo::CycleStatistics get_statistics(const std::string& name) const override;
};
```

Borrow the object through `RealtimeThreadConfig::cycle_manager` but still register/stop via the Facade:

```cpp
ExternalClock clock;
kairo::RealtimeThreadConfig config;
config.cycle_manager = &clock;
config.cycle_callback = [] { run_control_cycle(); };

executor.register_realtime_task("control", config);
executor.start_realtime_task("control");
// Keep clock alive through stopping.
executor.stop_realtime_task("control");
```

See [`examples/realtime_can.cpp`](https://github.com/Linductor-alkaid/kairo/blob/master/examples/realtime_can.cpp) for a minimal implementation.

## Inputs, lifecycle, and failure

`register_cycle()` receives a pre-bound `std::function<void()>`. Save it by value and invoke it when the cycle triggers; never retain a reference to the registration argument. Bind business input by value or stable owner in `config.cycle_callback`.

`cycle_manager` is a borrowed raw pointer. Stop realtime work and let `start_cycle()` return before destroying business objects or the source. If the source has helper threads, join them before destruction so none calls a destroyed callback.

- Do not start the cycle source after registration failure; inspect `ExecutorResult` and clean resources.
- `start_cycle()` reports startup failure without leaving a background thread.
- `stop_cycle()` wakes/ends waits so `stop_realtime_task()` is bounded.
- Callback respects realtime budget: no unbounded waiting, allocation, or uncontrolled locking; statistics report cycles, timeout, and running state.

An external trigger does not remove deployment checks for priority, affinity, memory locking, or permissions. Query realtime status and handle fallback.

Use built-in timing for ordinary maintenance and normal realtime loops. An external manager adds stop ordering, exception isolation, thread ownership, and test burden merely for “more precision.”

Next: [execution paths](/en/advanced/execution-paths).
