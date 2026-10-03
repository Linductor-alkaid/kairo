---
title: Performance Measurement and Regression Checks
description: Measure throughput, tail latency, jitter, and correctness with one protocol, then select PR, nightly, and release gates by environment stability.
---

# Performance Measurement and Regression Checks

## Write the question before running a benchmark

A performance test answers a concrete user question: how many specified parse tasks complete per second on this machine; whether 99% of requests reach a business result within 5 ms; p99 jitter of a 1 ms control cycle under target permissions/load; or whether sustained overload rejects by contract without losing accepted work. “Is Kairo fast?” is not an experiment. Fix workload, concurrency model, input size, queue policy, hardware, and pass condition before selecting metrics.

## Five result classes do not substitute for each other

| Dimension | Question | Recommended result | Does not prove |
| --- | --- | --- | --- |
| Correctness | Do acceptance, execution, failure, rejection, shutdown meet contract? | Completed/exceptions/rejections, data validation, TSAN/tests | Latency target |
| Throughput | How much valid work completes in steady window? | Completed ops/s, valid bytes/s | One request is timely |
| Tail latency | How long do typical/slow requests wait? | p50/p95/p99/max/sample count | Periodic wake stability |
| Jitter | How far actual time differs from expected time? | p50/p95/p99/max signed deviation | Task body is fast enough |
| Resource/backpressure | What did result cost, and what happens under overload? | CPU/memory/depth/drop/timeout ratio | Business result correctness |

Throughput counts **valid completions**. Attempting a million pushes with nine hundred thousand failures has no business throughput meaning. Latency reports include timeout/drop counts so incomplete requests do not disappear from the sample.

## Correctness precedes performance

Every experiment reconciles at least:

```text
attempted = accepted + rejected
accepted  = completed + failed + still_pending
```

After shutdown, `still_pending` becomes zero or is explicitly counted as abandoned by protocol. Track communication overwrites separately; realtime `dropped_task_count` includes detailed rejection counters and must not be double-added.

Do not omit unit/integration tests for values/exceptions/dependencies/shutdown, TSAN/stress for race/lifetime, checksum/business invariants, full-queue/handler-exception/stop-race reconciliation, or anti-optimization checks for empty workloads. An incorrect implementation's high throughput is not publishable.

## Unified experiment protocol

### Fix environment, workload, and noise

Record commit/dirty state; OS/kernel/architecture; CPU/sockets/physical/logical cores; allowed cpuset/affinity; memory; compiler/CMake/build flags; Kairo configuration; power governor/virtualization/container limits; realtime permissions/applied status; and GPU backend/driver/device if used.

Record task body/input size, producers/consumers/threads, queue capacity, batch size, period, per-cycle budget, drop policy, and duration. Keep everything but the measured variable constant. An empty lambda measures scheduling overhead, not production behavior; add a workload resembling real compute, memory access, and I/O boundaries.

Warm up outside the measured window for threads/pages/libraries. Repeat formal runs, retain each raw result, compare medians by default, and report worst run and dispersion. Request samples inside one run do not replace repeated experiments. On dedicated runners restrict background load, CPU set, and power policy; shared-CI p99 spikes are investigation signals, not cross-hardware promises. Preserve JSON, commands, stdout/stderr, and environment records as primary facts.

## Existing benchmark entry points

| User question | Entry | Main output | Suitable use |
| --- | --- | --- | --- |
| Facade submission/completion | `benchmark_baseline` | Submission throughput, future-wait distribution, end-to-end throughput, JSON | Version/config scan on fixed runner |
| Delayed/soft-periodic timing | `benchmark_timer_precision` | delayed/periodic jitter p50/p95/p99 | Background-timer environment baseline |
| Dedicated realtime period | `benchmark_realtime_precision` | Multi-cycle jitter p50/p95/p99 | Period acceptance on target platform/permission |
| MPSC contention/failure | `benchmark_lockfree_mpsc`, `benchmark_lockfree_mpsc_full` | Producer concurrency throughput/latency/failure | Internal queue research |
| Single-consumer aggregation | `benchmark_lockfree_task_executor` | Submission/execution throughput/latency | Advanced executor fit for a specific case |
| Batch policy | `benchmark_batch_*` | Throughput for batch/producer models | Compare batch parameters under fixed task body |

Historical `docs/performance/` reports provide optimization context, not a current-machine baseline or API performance guarantee.

`benchmark_baseline` submission throughput covers submission loop; end-to-end includes submit/task/wait. Its current `round_trip_latency` bulk-submits before recording each `get()` wait, so it measures remaining wait at `get()`, not submit-to-completion latency per item. For a request SLO timestamp each submission and end when business owner consumes its result. Timer/realtime precision benchmarks use quick defaults; release acceptance increases cycles under target load and records timeout/drop, applied platform fields, real callback/handler cost, and scheduling noise.

## Reproducible local baseline

```bash
cmake -S . -B build-perf \
  -DCMAKE_BUILD_TYPE=Release \
  -DKAIRO_BUILD_TESTS=ON \
  -DKAIRO_BUILD_EXAMPLES=OFF \
  -DKAIRO_ENABLE_GPU=OFF
cmake --build build-perf -j --target \
  benchmark_baseline \
  benchmark_timer_precision \
  benchmark_realtime_precision

./build-perf/tests/benchmark_baseline \
  --json --tasks 50000 --min-threads 4 --max-threads 8 \
  --queue-capacity 10000
./build-perf/tests/benchmark_timer_precision \
  --json --tasks-per-period 200 --cycles-per-period 200
./build-perf/tests/benchmark_realtime_precision \
  --json --cycles-per-period 1000
```

For Windows multi-config builds use `--config Release` and `ctest -C Release`; realtime results must be recreated under final account/permissions. `ctest --test-dir build-perf -N -L benchmark` checks registration, and `ctest -L benchmark` checks runnable defaults, but formal results retain explicit executable commands and parameters.

## Metric definitions

```text
throughput = completed_valid_work / measured_seconds
jitter = actual_timestamp - expected_timestamp
```

State whether the measured window includes initialization, warmup, submission, drain, and shutdown. Separate submission from end-to-end throughput. Sort semantically identical item samples for p50/p95/p99 and report sample count/timeouts/max; confirm identical percentile algorithms and recognize that small-sample p99 may be one observation. Preserve jitter sign to distinguish early/late (or separately calculate absolute jitter), and state expected schedule origin.

For larger-is-better throughput:

```text
regression = (baseline - candidate) / baseline
```

For smaller-is-better latency/jitter:

```text
regression = (candidate - baseline) / baseline
```

Baselines require the same runner/config/traceable commit. Fail only beyond an observed historical noise band—not an intuitive universal 5%.

## Three regression-gate layers

**PR correctness gate:** stable unit/integration/concurrency/tutorial smoke tests, targeted TSAN/ASAN, benchmark compile/minimal sample, and completion/rejection/exception/shutdown invariants. Shared-runner absolute throughput/p99 typically record but do not block.

**Nightly or dedicated-runner trend gate:** fixed hardware/image/CPU policy, repeated time series, alert after several candidate results cross historical noise; rerun one spike and inspect host noise. Compare throughput, tails, drops, and CPU together so more resources cannot mask regression.

**Release user-SLO gate:** target SKU/permission/driver/near-production workload verifies absolute end-to-end latency, jitter, sustained throughput, overload behavior, resource limits, and shutdown budget. Nightly relative trends cannot replace target-environment acceptance.

## Result record

```markdown
## Question and conclusion
- User question:
- Conclusion:
- Applicability boundary:

## Environment
- commit / dirty state:
- OS / CPU / allowed cpuset / memory:
- compiler / CMake / Release flags:
- permissions / GPU:

## Workload
- task body / input size:
- producers / consumers / threads:
- queue / batch / period / drop policy:
- warm-up / duration / repetitions / samples:

## Correctness
- attempted / accepted / completed / failed / rejected:
- checksum / invariants / sanitizer:

## Performance
- throughput:
- p50 / p95 / p99 / max / timeout:
- jitter definition and percentiles:
- CPU / memory / queue / drop:

## Comparison and gate
- baseline commit and raw artifact:
- per-run results and aggregate:
- noise band / threshold / decision:
- follow-up:
```

Link raw output and reproduction commands. Do not use the single best run, a result without correctness reconciliation, or an undocumented environment to change public performance claims.

Return to [lock-free experiments](/en/advanced/lockfree-and-performance) for advanced executor evaluation; use [capacity and alerts](/en/realtime-and-communication/capacity-and-alerting) to turn measured capacity into runtime action.
