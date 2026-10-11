---
title: Translation Status
description: Published English coverage and the synchronization status for Chinese documentation.
sidebar: false
---

# Translation Status

English pages use the same repository examples and commands as Chinese pages; no language-specific code copies are maintained. API names, error codes, configuration fields, paths, and commands remain in English so they are searchable against the source and reference documentation.

## Published English paths

| Chinese route | English route | Status | Fact source |
| --- | --- | --- | --- |
| `/` | `/en/` | Published | Repository README and tutorial example `01` |
| `/zh/getting-started/what-is-kairo` | `/en/getting-started/what-is-kairo` | Published | Repository README capability boundaries, public Facade, and website design guidance |
| `/zh/quick-start/build` | `/en/quick-start/build` | Published | `docs/BUILD.md` |
| `/zh/quick-start/first-task` | `/en/quick-start/first-task` | Published | `examples/tutorial/01_first_task.cpp` |
| `/zh/guides/execution-models-and-routing` | `/en/guides/execution-models-and-routing` | Published | Unified Facade routing and result-model public APIs |
| `/zh/guides/adaptive-scheduling` | `/en/guides/adaptive-scheduling` | Published | `include/kairo/adaptive_scheduler.hpp`, `docs/design/scheduling_runtime.md` §8, `docs/performance/m3_adaptive_scheduler_results.md` |
| `/zh/quick-start/task-inputs-and-ownership` | `/en/quick-start/task-inputs-and-ownership` | Published | `examples/tutorial/11_task_inputs.cpp` |
| `/zh/quick-start/return-values-and-errors` | `/en/quick-start/return-values-and-errors` | Published | `std::future` and tutorial example `01` |
| `/zh/quick-start/lifecycle` | `/en/quick-start/lifecycle` | Published | Public lifecycle API and `docs/API.md` |
| `/zh/tutorial/` | `/en/tutorial/` | Published | `examples/tutorial/02`–`05`, `10`, and `comm_robot_pipeline` |
| `/zh/guides/` | `/en/guides/` | Published | Public Facade/communication APIs, tutorials, and production guidance |
| `/zh/realtime-and-communication/` | `/en/realtime-and-communication/` | Published | Realtime, communication, and blocking-I/O public APIs; tutorial examples `07`–`08`, `12` |
| `/zh/realtime-and-communication/blocking-io-workers` | `/en/realtime-and-communication/blocking-io-workers` | Published | `IBlockingIoWorker`, Facade lifecycle API, and tutorial example `12` |
| `/zh/realtime-and-communication/cancellation-and-timers` | `/en/realtime-and-communication/cancellation-and-timers` | Published | Cancellation/timer public APIs, serial dispatch semantics, and tutorial example `13` |
| `/zh/realtime-and-communication/capacity-and-alerting` | `/en/realtime-and-communication/capacity-and-alerting` | Published | Bounded comm/realtime capacity plus `max_in_flight_tasks` total admission (`docs/API.md` §3.10) |
| `/zh/guides/event-loop-interop` | `/en/guides/event-loop-interop` | Published | `docs/external_event_loop_interop.md` and `examples/event_loop_interop.cpp` |
| `/zh/reliability/` | `/en/reliability/` | Published | Failure/status APIs, monitoring, deployment checks, and tutorial example `06` |
| `/zh/advanced/` | `/en/advanced/` | Published | Public advanced APIs, current source architecture, and benchmark protocol |
| `/zh/gpu/` | `/en/gpu/` | Published | GPU public APIs, backend diagnostics, and tutorial example `09` |
| `/zh/reference/version-and-migration` | `/en/reference/version-and-migration` | Published | `CHANGELOG.md`, `docs/MIGRATION.md` |
| `/decisions` | `/en/decisions` | Published | `website/decisions.md` |
| `/maintenance` | `/en/maintenance` | Published | `website/maintenance.md` |

## Pending topic groups

| Chinese route group | English status | Synchronization rule |
| --- | --- | --- |
| `/zh/quick-start/` | Complete | All six Chinese quick-start pages have an English counterpart. |
| `/zh/tutorial/` | Complete | All eight Chinese tutorial pages have English counterparts using the same C++ examples and commands. |
| `/zh/guides/` | Complete | All seven Chinese scenario and production guides have English counterparts. |
| `/zh/realtime-and-communication/` | Complete | All seven Chinese real-time and communication pages have English counterparts. |
| `/zh/reliability/` | Complete | All five Chinese reliability pages have English counterparts. |
| `/zh/advanced/` | Complete | All seven Chinese advanced and internal-behavior pages have English counterparts. |
| `/zh/gpu/` | Complete | All four Chinese GPU pages have English counterparts. |
| `/zh/reference/api` | Linked source | Keep `docs/API.md` as the single complete signature reference. |

## Fallback behavior

The language control opens the exact translated counterpart when it is published. On a Chinese page without an English counterpart, it opens the English home page and labels the destination as a home fallback; no empty English page is published. English pages always link back to their Chinese counterpart.

The shared 404 renderer uses the requested URL prefix: `/en/...` displays English recovery links, while other missing routes display Chinese recovery links. `writing-template.md` is an internal authoring template rather than published reader content, so it has no English mirror.

When a Chinese page adds a public capability, update this table to `Needs translation` or add the published English route in the same change. Release review must check this table together with the API and migration documents.
