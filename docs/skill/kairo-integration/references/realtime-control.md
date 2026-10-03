# Realtime Control

## Use It For

A fixed-period control loop with explicit cycle configuration, bounded command intake, and observable drop/timeout state. For ordinary maintenance use periodic tasks instead.

## Minimal Usage

```cpp
executor::RealtimeThreadConfig config;
config.thread_name = "control";
config.cycle_period_ns = 2'000'000;
config.cycle_callback = [] { run_control_cycle(); };

if (!executor.register_realtime_task_ex("control", config)) return 1;
if (!executor.start_realtime_task_ex("control")) return 1;
const bool queued = executor.try_push_realtime_task("control", [] { apply_command(); });
executor.stop_realtime_task("control");
```

## Integration Pitfalls

- `queued` means bounded-queue admission only, not command completion. Inspect realtime status and application state for execution evidence.
- Do not block, allocate unpredictably, or drain unbounded work inside `cycle_callback`. `max_tasks_per_cycle` protects the period by leaving excess work for later cycles.
- Process memory locking is disabled by default because Linux `mlockall` affects the entire process and later mappings. Scheduling priority, affinity, and memory-lock requests can be denied by platform permissions; inspect status fields.
- Emergency hardware safety must bypass this queue. A cycle and OS scheduling are not absolute deadline guarantees.

## Related Guide

`website/en/realtime-and-communication/realtime-control.md`.
