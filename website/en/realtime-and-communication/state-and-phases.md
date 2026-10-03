---
title: Latest Values, Snapshots, and Phases
description: Choose LatestMailbox, DoubleBuffer, PhaseGate, or Sequencer for current configuration, complete state, and startup order.
---

# Latest Values, Snapshots, and Phases

## Goal

Choose `LatestMailbox<T>`, `DoubleBuffer<T>`, `PhaseGate`, or `Sequencer` by data semantics instead of assembling synchronization from a shared object and flags.

## Minimal pipeline

<<< @/../examples/tutorial/08_communication.cpp{1-29}

```bash
./build/examples/tutorial/tutorial_08_communication
```

```text
frame=7, gain=3, state=21, phase=ready
```

## Retain only current configuration

`LatestMailbox<T>` retains the most recent `publish()` value. A real-time consumer uses a sequence to avoid reusing an old configuration:

```cpp
uint64_t seen = 0;
ControlConfig config;
if (mailbox.try_load_newer_than(seen, config, seen)) {
    apply_config(config);
}
```

Overwriting a prior configuration increments `overwritten_count`; no higher sequence is a stale read, not a lost new message. `publish(value)` copies an lvalue and `publish(std::move(value))` moves it into the mailbox; readers copy the current value to their output object. The mailbox never keeps a reference to a publisher local, but pointers/views inside `T` still need application-managed backing lifetime. For a large immutable configuration, use `shared_ptr<const Config>` after complete validation.

The mailbox uses four fixed reader-pinned snapshot slots. A writer claims only an unpinned slot, so a reader can copy a non-trivial `T` without racing a rewrite. `try_load()` checks at most four slots. `try_publish(value, &sequence)` is non-waiting and system-wide lock-free, but its publication CAS may retry while another publisher advances, so it is not per-call bounded or wait-free. It returns `false` when all slots are temporarily busy or the finite sequence domain is exhausted. The compatibility `publish()` retries temporary contention with `yield()`; it throws `std::overflow_error` on permanent sequence exhaustion instead of spinning forever.

## Publish complete state

`DoubleBuffer<T>` has a single-writer/multiple-reader contract. Its ordinary mode also uses four fixed reader-pinned slots: `publish()` or `update()` completes a candidate before publishing its sequence, while a reader pins a stable slot and copies a value `Snapshot<T>`. It never exposes a partially updated object and does not acquire a mutex. Funnel multiple writers through an `MpscChannel` to one state owner, and assess copying cost for large state.

Use `try_load()` when a fixed four-slot read attempt is required. `try_publish()` is a non-waiting lock-free publish interface, but not a per-call bounded/wait-free one. `publish()`, `load()`, and `update()` preserve compatibility behavior by spin/yield retrying temporary contention; they are control-plane calls rather than real-time operations. The snapshot sequence ends at `2^56 - 1`: `try_publish()` then returns `false`, while publish/update compatibility paths throw `std::overflow_error`.

`update()` copies the current complete snapshot into a writer-local candidate, modifies that candidate synchronously, and then publishes it; it is not an asynchronous Kairo submission. Its references need only cover that immediate call, but it still obeys the one-writer constraint. A reader's snapshot remains its own copy after later publication.

## Phases and publication watermarks

Use `PhaseGate` for monotonic setup/calibration/running stages. `advance_to()` cannot repeat or regress; `wait_for()` distinguishes success, `Timeout`, and `Closed`; `wait_for_exact()` also exposes a skipped stage as `MissedPhase`.

Use `Sequencer` for a monotonic publication watermark. `next_ticket()` allocates increasing tickets, but `publish(ticket)` may jump directly to a larger ticket. `is_published(ticket)` means the watermark reached or passed it; it does not prove that exact ticket was individually published. `wait_until_published(ticket, timeout)` is an exact wait and returns `MissedPhase` once the watermark passes the target. It is not a data queue and cannot replace `MpscChannel`.

Both types use nonblocking atomic state cores and reject construction if their required synchronization atomics are not lock-free. Their timeout waits poll the core with `steady_clock` and `yield()`: use them for setup/control coordination, not inside a hard-real-time cycle.

The closed flag occupies the high state bit, so phase and ticket values must be below `2^63`. Phase waits reject `phase >= 2^63`, and sequencer waits reject ticket `0` or `ticket >= 2^63`, immediately with `InvalidArgument` before clock access or polling. `next_ticket()` returns `0` after close or ticket-space exhaustion. Phase-bound LET reserves `2^63 - 1` as its empty-slot sentinel, so publication is valid only while `phase < 2^63 - 1`.

## Phase-bound LET values

When a reader must reason about logical time, explicitly bind a `DoubleBuffer<T>` or
`LatestMailbox<T>` to the gate. This is an optional mode of existing components, not a separate
`LetChannel<T>` API:

```cpp
kairo::comm::PhaseGate gate;
kairo::comm::DoubleBuffer<ControlState> state(ControlState{});
state.bind_to_phase_gate(gate);

state.publish_for_current_phase(ControlState{/* phase 0 output */});
gate.advance();

kairo::comm::Snapshot<ControlState> visible;
if (state.load_for_current_phase(visible)) {
    consume(visible.value); // phase 1 sees the complete phase 0 output.
}
```

The bound contract is SWSR with fixed two-slot storage. Each phase accepts at most one publish;
the current phase cannot read its own in-progress value, and a missing prior value or a competing
advance/read/write reports `CommResult::NotReady`. `LatestMailbox<T>` uses the same phase APIs,
but becomes a one-value-per-phase snapshot in bound mode. Its ordinary unbound `publish()` /
`try_load()` API remains latest-wins.

`RealtimeChannel<T>` does not inherit LET: FIFO cycle budgets and a phase-bound single value are
different contracts. In bound mode, successful periodic operations use the fixed slots without a
mutex, condition-variable wait, or internal-storage allocation; failure diagnostics are outside
that successful path.

`is_synchronization_lock_free()` is deliberately narrower than “hard real-time”: it covers the
component's internal synchronization atomics, not copying/moving `T`, clocks, strings/results,
callbacks, allocation performed by `T`, page faults, or OS scheduling. Fixed-attempt reads,
non-waiting system-wide lock-free publication, allocation-free internal storage, data-race
freedom, and a measured hard-real-time path are separate claims. Configure and invoke event
callbacks only on the diagnostic/control plane.

Next: [communication observability](/en/realtime-and-communication/observability).
