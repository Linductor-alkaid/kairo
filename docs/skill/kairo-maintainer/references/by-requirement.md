# Index By Requirement Or Constraint

| Requirement, risk, or failure symptom | Read first | Key question |
| --- | --- | --- |
| Need a return value or task exception | [General submission](capabilities/general-submission.md) | Is a `future` completion required? |
| Need an acceptance result rather than completion | [Routing](capabilities/routing.md) | Is the backend bounded or independently owned? |
| Work must stop promptly | [Facade and lifecycle](capabilities/facade-lifecycle.md) | Who owns stop, wait, and join? |
| Work may block on external I/O | [Blocking I/O workers](capabilities/blocking-io.md) | What wakes the blocked call? |
| Need bounded queueing, drop behavior, or cycle budget | [Realtime control](capabilities/realtime-control.md) | Is rejection/drop observable to the caller? |
| Need a latest value, FIFO message, immutable snapshot, or phase ordering | [Communication primitives](capabilities/communication.md) | What delivery semantics does the data require? |
| Need independent consumers to receive the same subsequent events | [Communication primitives](capabilities/communication.md) | What is each subscriber's capacity and loss policy? |
| Need priority, delay, periodic work, batch throughput, or dependencies | [Scheduling and task graphs](capabilities/scheduling-task-graph.md) | Which scheduling semantic is actually required? |
| Need latency/throughput improvement | [Concurrency internals](capabilities/concurrency-performance.md) | What correctness invariant and measurement gate prove it? |
| Need diagnostics, counters, alerts, or a bounded wait | [Observability](capabilities/observability.md) | Which state surface exposes the failure? |
| GPU may be unavailable or must fall back to CPU | [GPU execution](capabilities/gpu.md) | Is CPU fallback explicitly permitted? |
| Possible stop/submit race, lost future, UAF, or deadlock | [Facade and lifecycle](capabilities/facade-lifecycle.md) | Which owner and lifetime proof covers the race? |
