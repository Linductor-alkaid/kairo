# Index By Requirement

| Requirement or concern | Read |
| --- | --- |
| Need a value or task exception | [Tasks and lifecycle](tasks-and-lifecycle.md) |
| Need a timeout but cannot safely kill C++ work | [Tasks and lifecycle](tasks-and-lifecycle.md) |
| Need ordering, a delay, repeat work, batching, or dependencies | [Scheduling](scheduling.md) |
| Need bounded latency, a cycle budget, CPU affinity, or realtime priority | [Realtime control](realtime-control.md) |
| Need to stop an external blocking read promptly | [Blocking I/O](blocking-io.md) |
| Must deliver every message, keep only the latest value, or share a full snapshot | [Communication](communication.md) |
| Need CPU fallback when a GPU is unavailable | [GPU](gpu.md) |
| Need bounded fire-and-forget admission or a named low-latency queue | [Routing and low-latency](routing-low-latency.md) |
| Need alerts, failure events, status, or a support snapshot | [Observability](observability.md) |
| Need an external clock or a custom backend | [Advanced extensions](advanced-extensions.md) |
| Need a downstream AI to see this skill | [Adoption](adoption.md) |

Choose the smallest capability that satisfies the requirement. A timeout is not cancellation, priority is not preemption, and queue admission is not task completion.
