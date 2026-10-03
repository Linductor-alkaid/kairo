# Communication

## Use It For

Passing data across threads. Choose the component from delivery semantics, not expected speed.

## Minimal Usage

```cpp
#include <executor/comm.hpp>

executor::comm::MpscChannel<int> commands({.capacity = 64});
if (!commands.try_send(7)) handle_full_queue();

int command = 0;
if (commands.try_receive(command)) apply(command);
```

Use `LatestMailbox<T>` when only the newest value matters, `MpscChannel<T>` when every item is FIFO, `RealtimeChannel<T>` when a cycle consumes a bounded number of messages, `DoubleBuffer<T>` for complete snapshots, `PhaseGate` for monotonic phases, and `Sequencer` for a monotonic publication watermark that may skip tickets.

## Phase And Sequence Usage

```cpp
executor::comm::Sequencer sequencer("startup");
const auto ticket = sequencer.next_ticket();
if (!sequencer.publish(ticket)) handle_sequence_error();
if (!sequencer.wait_until_published(ticket, std::chrono::seconds(1))) handle_sequence_error();
```

Bind `DoubleBuffer<T>` or `LatestMailbox<T>` to a `PhaseGate` only when a value must become visible at a later logical phase. Use `RealtimeChannel<T>` when the consumer has an explicit per-cycle drain budget; it is not a replacement for the Executor realtime task queue.

## Integration Pitfalls

- A full channel has a policy: reject, drop oldest, or keep latest. Observe a send result, `CommStats`, or an event callback rather than assuming delivery.
- `LatestMailbox` overwrites old values by design; it is not FIFO. A `DoubleBuffer` is a state snapshot, not a message queue.
- Communication events and statistics do not automatically enter `ExecutorFailureStatus`; bridge their callbacks into service monitoring when needed.
- Preallocated channels and reader-pinned snapshots provide lock-free internal synchronization, not a whole-path hard-realtime guarantee. Snapshot `try_publish()` is system-wide lock-free but not per-call bounded/wait-free; `try_load()` has four slot attempts.
- `Topic<T>`, including publish fan-out, uses a mutex and dynamic allocation and is explicitly non-realtime.
- `RealtimeAllocationGuard` is an opt-in allocation diagnostic, not an allocator or a realtime scheduling guarantee.

## Related Guide

`website/en/guides/choosing-communication.md` and `website/en/realtime-and-communication/state-and-phases.md`.
