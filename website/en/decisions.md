---
title: Launch Decisions
description: Frozen website launch decisions, content sources, and tutorial ownership.
sidebar: false
aside: false
---

# Launch Decisions

| Item | Decision | Reason |
| --- | --- | --- |
| Site name | Kairo Guide | Matches the repository name and clearly signals a learning resource. |
| Audience promise | Complete a first observable asynchronous task in ten minutes | The shortest successful MVP path. |
| Source directory | `website/` | Separates the site from C++ source, examples, and reference documents. |
| Toolchain | VitePress 1.6.3, Node.js 20 LTS, npm | Static generation, built-in search, and low maintenance. |
| URL | GitHub Pages project site at `/kairo/`; no custom domain initially | Matches `Linductor-alkaid/kairo` and avoids launch DNS dependency. |
| Language policy | Chinese root home page; `/zh/` and `/en/` content trees | Root never redirects by browser language, preserving predictable deep links. |
| Version policy | Stable `v0.7.0` baseline | Pages follow current repository API; unreleased features are not stable-version promises. |
| Diagram policy | Markdown text diagrams initially | Avoids extra Mermaid dependency and build risk. |
| Code source | `examples/tutorial/`, embedded with VitePress `<<< @` | Core page snippets point at compiled examples, avoiding duplicate facts. |
| English launch | Publish only complete English counterparts | Never publish empty translations; maintain one-to-one published routes. |
| API reference | Link to `docs/API.md` | Avoids maintaining a duplicate complete API reference. |

## Content migration inventory

| Existing content | Website role | Launch treatment | Maintenance owner |
| --- | --- | --- | --- |
| `README_zh.md` / `README.md` | Project overview and entry | Home-page summary; README remains repository entry | Project maintainer |
| `docs/BUILD.md` | Build facts | Reorganized in Quick Start and linked | Build maintainer |
| `docs/API.md` | API facts/reference | Tutorials cite by scenario; reference links directly | API maintainer |
| `docs/MIGRATION.md` / `CHANGELOG.md` | Versioning and migration | Version/migration page links directly | Release maintainer |
| `docs/design/*.md` | Design and internals facts | Aggregated by Advanced pages | Architecture maintainer |
| `docs/performance/*.md` | Performance facts | Referenced by GPU, batch, and lock-free topics | Performance maintainer |
| `examples/` | Scenario/platform facts | Remain in place and are linked by topic | Example maintainer |
| `examples/tutorial/` | Tutorial code facts | Embedded in pages, built by CMake smoke tests | Tutorial maintainer |

## Initial page tracking

| Page | Fact source | Complete example | Acceptance owner |
| --- | --- | --- | --- |
| Home | `README_zh.md`, public Facade | `examples/tutorial/01_first_task.cpp` | Project maintainer |
| What is Kairo? | `docs/design/user_guide_website.md` | — | Architecture maintainer |
| Build and install | `docs/BUILD.md`, root `CMakeLists.txt` | `01_first_task.cpp` | Build maintainer |
| Your first task | `include/kairo/executor.hpp` | `01_first_task.cpp` | Tutorial maintainer |
| Return values and errors | `Executor::submit_auto()`, future | `01_first_task.cpp` | Tutorial maintainer |
| Initialization and shutdown | `Executor::initialize()`, `shutdown()` | `01_first_task.cpp` | API maintainer |
| Versions and migration | `docs/MIGRATION.md`, `CHANGELOG.md` | — | Release maintainer |

New navigable pages must identify a fact source, example where applicable, and acceptance owner before entering navigation.

## Tutorial model

The tutorial sequence shares a robot data pipeline: `SensorFrame`, `ParsedFrame`, `Plan`, `ControlCommand`, `ControlConfig`, and `SystemState`.

1. Parse `SensorFrame` in the background with `submit_auto(lambda)`.
2. Let `ControlCommand` queue before ordinary analysis with `submit_priority()`.
3. Retry an unavailable device and run health checks with `submit_delayed()` / `submit_periodic()`.
4. Process frames in parallel and combine `Plan` using batch, `TaskHandle`, and `when_all()`.
5. Transfer `ControlConfig`, commands, and `SystemState` to periodic control through communication Facade components.
6. Enter real-time threads only for strict periods; enter GPU only for demonstrated computational benefit.
