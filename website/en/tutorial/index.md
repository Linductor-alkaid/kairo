---
title: Step-by-Step Tutorials
description: Learn the Kairo Facade through a robot data pipeline and a server-side import.
---

# Step-by-Step Tutorials

The tutorials follow one robot data pipeline: acquire `SensorFrame`, produce a `ParsedFrame`, create a `Plan`, then send a `ControlCommand` to a control loop. Each chapter introduces one business problem and only the APIs needed to solve it.

Start with [your first task](/en/quick-start/first-task) and [submitting functions and data](/en/quick-start/task-inputs-and-ownership), then proceed through:

1. [Prioritize control commands](/en/tutorial/priority): urgent control and ordinary analysis share a pool.
2. [Delayed retry and health checks](/en/tutorial/delayed-and-periodic): retry later and stop observable soft-periodic work.
3. [Batch sensor frames](/en/tutorial/batch): choose a batch path based on whether each task needs a future.
4. [Load, sense, then plan](/en/tutorial/dependencies): run planning only after prerequisite work completes.
5. [Bounded waiting and status](/en/tutorial/waiting-and-status): finish safely before a phase change or shutdown.
6. [Fan out events with Topic](/en/tutorial/topic-subscriptions): give independent consumers separate bounded queues, backpressure policies, and shutdown ownership.
7. [Complete robot pipeline](/en/tutorial/complete-robot-pipeline): connect startup dependencies, frame streams, configuration, commands, snapshots, diagnostics, and shutdown.
8. [Service data import](/en/tutorial/service-data-import): apply the same dependency, batch, partial-failure, and bounded-drain model to a server request.

Every page identifies scale and concurrency assumptions, ownership of asynchronous objects, failure injection, in-flight work during exit, and when changing requirements require a different abstraction. Treat those sections as an architecture-review checklist.

For API selection by problem, see [Choose a Submission API](/en/guides/choosing-submit-api) and [Execution Models and Routing Boundaries](/en/guides/execution-models-and-routing). Enter real-time, GPU, lock-free, or Blocking I/O topics only when their constraints are explicit.
