---
name: kairo-integration
description: Integrate the Executor C++20 library into an application. Use when adding asynchronous tasks, initialization and shutdown, results and errors, priority/delayed/periodic work, realtime control, blocking I/O workers, cross-thread communication, or optional CUDA/OpenCL work to an application that depends on Executor.
---

# Executor Integration

Use this skill as an application developer. Public headers and the current user guide are authoritative. Load one router and one card only; do not read unrelated cards or implementation sources.

## Route The Request

| Request contains | Read exactly this next |
| --- | --- |
| First integration, initialization, `submit`, `future`, or `shutdown` | [Quick start](references/quick-start.md) |
| An application feature or workload | [By scenario](references/scenarios.md) |
| A requirement, constraint, or failure concern | [By requirement](references/by-requirement.md) |
| A known API, type, status, or error term | [By API](references/by-api.md) |

After a router selects a card, implement its minimal usage, preserve its integration pitfalls, and build the application. Observe success through the card's named future, result, status, or callback; do not infer completion from queue admission.

## Downstream Use

Read [adoption](references/adoption.md) only when the AI runs from a downstream project and cannot already access this skill. Do not load implementation internals unless reproducing a library defect.
