---
title: Choose a Message-Passing Component
description: Use MpscChannel for ordinary data flow and a budgeted RealtimeChannel for periodic consumption.
---

# Choose a Message-Passing Component

## Goal

Distinguish “every frame must be handled” from “a real-time cycle may handle only a bounded number of commands,” then choose `MpscChannel<T>` or `RealtimeChannel<T>` accordingly.

## Ordinary flow: `MpscChannel`

A frame stream from acquisition to planning commonly requires FIFO consumption. `MpscChannel<T>` is a bounded multi-producer/single-consumer channel; an ordinary consumer can use `receive_for()` to bound its wait.

```cpp
kairo::comm::ChannelOptions options;
options.capacity = 256;
options.drop_policy = kairo::comm::DropPolicy::RejectNewest;

kairo::comm::MpscChannel<SensorFrame> frames(options);
frames.try_send(SensorFrame{});

SensorFrame frame;
if (frames.receive_for(frame, std::chrono::milliseconds(10))) {
    plan(frame);
}
frames.close();
```

The channel preallocates its bounded node pool during construction. Producers construct complete values in privately claimed nodes before atomic publication; one logical consumer detaches complete batches and preserves FIFO publication order. `try_send()` and `try_receive()` are non-waiting entry points that acquire no mutex and allocate no queue storage; contended atomic retries are lock-free, not wait-free. `send_for()` and `receive_for()` spin/yield over that core and use `CommResult` to distinguish `Timeout`, `Closed`, and other outcomes, so they belong on ordinary control threads. `close()` stops new production, and already queued data can still drain.

## A channel transfers values, not task functions

Communication components have no callable/argument binding model. Producers submit a `T`: lvalues copy, rvalues move. After successful sending, the consumer owns the channel's message rather than a reference to the producer stack.

```cpp
SensorFrame frame = capture_frame();
if (!frames.try_send(std::move(frame))) {
    handle_backpressure();
}
```

Do not rely on the original contents after moving. A raw pointer, span, or view transfers only that lightweight object, not its backing storage; the application keeps the buffer valid until consumption. Prefer a buffer handle with defined return rules or a smart pointer, and define pool-exhaustion backpressure.

## Real-time cycle: `RealtimeChannel`

A real-time thread must not wait on a condition variable or clear unlimited backlog. Bound per-cycle work with `drain_for_cycle()`:

```cpp
kairo::comm::RealtimeChannelOptions options;
options.capacity = 128;
options.max_items_per_cycle = 8;

kairo::comm::RealtimeChannel<ControlCommand> commands(options);
commands.try_send(ControlCommand{});
commands.drain_for_cycle([](const ControlCommand& command) {
    apply_command(command);
});
```

`max_items == 0` uses configured `max_items_per_cycle`; only a configuration value of `0` means unlimited. Preserve a production bound so backlog cannot consume the entire control period. A handler exception stops this drain, increments `handler_exception_count`, emits `HandlerException`, and continues propagating the exception.

`RealtimeChannel` uses the same construction-time preallocated MPSC core and one-logical-consumer contract. `try_send()`, `drain_for_cycle()`, close, and state queries acquire no mutex or queue-storage allocation. Construction throws rather than silently falling back when the required pointer/integer atomics are not lock-free. `is_synchronization_lock_free()` reports only that internal synchronization property.

This still does not make the whole call hard real-time. Copying/moving/destructing `T`, `steady_clock`, the handler, exception propagation, diagnostic event strings/callbacks, page faults, and OS scheduling are outside the guarantee. Configure callbacks only on the diagnostic/control plane; invoke the cycle with bounded, non-throwing payload operations and handler code, then validate the complete path on the target platform. Lock-free also does not mean every contended operation is wait-free.

The handler receives `const T&` valid only for that call. Copy or transfer it to an owned object before retaining it asynchronously. Keep the handler bounded and nonblocking because it runs inside the real-time cycle.

| Policy | Meaning | Typical use |
| --- | --- | --- |
| `RejectNewest` | Reject new message; producer observes failure | Every message matters; caller applies backpressure |
| `DropOldest` | Discard oldest, accept newest | Newer data matters more but remains queue semantics |
| `KeepLatest` | Retain only newest message | Near state semantics; `LatestMailbox` may fit directly |

Never ignore `try_send()`. Capacity, drop policy, peak depth, and drop count form a backpressure contract, not a performance value to increase arbitrarily. For latest-only configuration, continue with [latest values, snapshots, and phases](/en/realtime-and-communication/state-and-phases).
