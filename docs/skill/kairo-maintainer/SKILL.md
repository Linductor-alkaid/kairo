---
name: kairo-maintainer
description: Maintain the Executor C++20 library with a source-first map of its public contracts, execution paths, concurrency invariants, tests, and documentation. Use when exploring this repository, choosing an Executor capability for a feature, changing public or internal behavior, debugging a lifecycle/concurrency issue, adding tests, or updating maintainer documentation.
---

# Executor Maintainer

Use this repository-local skill before changing Executor behavior. It is a navigator, not a frozen API copy: declarations, implementation, and tests remain authoritative.

## Startup

1. Read [the knowledge-map design](references/index.md).
2. Route the request through exactly one primary index; use a second index only when the first does not answer the question.
3. Read the linked capability card, then the declared header, implementation, and test before proposing or editing code.
4. Treat the card's invariants and verification targets as mandatory review points. Re-check the source when a card disagrees with it, then update the card in the same change.

## Choose An Index

- Business outcome or product capability: [by business feature](references/by-business-feature.md).
- Requirement, constraint, failure mode, or non-functional goal: [by requirement](references/by-requirement.md).
- Symbol, subsystem, source file, or test failure: [by implementation](references/by-implementation.md).

## Capability Cards

Load only the card needed for the current change. The cards are intentionally small and separately maintained.

- [Facade and lifecycle](references/capabilities/facade-lifecycle.md)
- [General submission and completion](references/capabilities/general-submission.md)
- [Scheduling and task graphs](references/capabilities/scheduling-task-graph.md)
- [Scheduling Runtime](references/capabilities/scheduling-runtime.md)
- [Routing and named backends](references/capabilities/routing.md)
- [Realtime control](references/capabilities/realtime-control.md)
- [Blocking I/O workers](references/capabilities/blocking-io.md)
- [Communication primitives](references/capabilities/communication.md)
- [Observability and diagnostics](references/capabilities/observability.md)
- [GPU execution](references/capabilities/gpu.md)
- [Concurrency internals and performance](references/capabilities/concurrency-performance.md)

## Maintain The Map

For a behavior change, add or revise the smallest affected card and every matching index entry. A card must state its public boundary, source of truth, observable failure behavior, tests, and one or more search terms. Do not paste complete API listings or duplicate user-guide prose.
