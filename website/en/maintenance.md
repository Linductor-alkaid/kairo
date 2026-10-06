---
title: Content Maintenance
description: Fact sources, review cadence, feedback route, and content-maturity criteria for the Kairo Guide.
sidebar: false
aside: false
---

# Content Maintenance

This guide is complete when users can finish work and handle failure, not merely when every public API appears. The current baseline is `v0.6.1` plus the later `master` development snapshot; pending features are not stable-release commitments.

<div class="maintenance-hero">
  <p class="maintenance-eyebrow">CURRENT BASELINE</p>
  <p class="maintenance-version">v0.6.1 + master development snapshot</p>
  <p>Recheck the responsible area whenever a stable tag, public API behavior, or tutorial example changes.</p>
</div>

## How content stays trustworthy

<div class="maintenance-grid">
  <section class="maintenance-card maintenance-card-coral">
    <span>01</span>
    <h3>Tutorial code executes</h3>
    <p>Core snippets originate in <code>examples/tutorial/</code> and are continuously compiled and smoke-tested by CMake.</p>
  </section>
  <section class="maintenance-card maintenance-card-cream">
    <span>02</span>
    <h3>Behavior returns to facts</h3>
    <p>Signatures and semantics come from public headers, tests, and API documentation; the site explains user decisions rather than duplicating a reference manual.</p>
  </section>
  <section class="maintenance-card maintenance-card-forest">
    <span>03</span>
    <h3>Recheck at release</h3>
    <p>Version, build, migration, performance, and platform differences are synchronized with release review rather than carried forward untested.</p>
  </section>
</div>

## Responsibility and facts

| Area | Pages | Primary fact source | Recheck when |
| --- | --- | --- | --- |
| Quick Start and ordinary work | Home, `zh/quick-start/`, `zh/tutorial/` | `include/kairo/executor.hpp`, tutorials `01`–`06` | Facade, future, waiting, or lifecycle changes |
| Reliability | `zh/reliability/` | Failure/status types, relevant tests, tutorial `06` | Event type, counter, or monitoring semantics change |
| Real-time and communication | `zh/realtime-and-communication/` | Realtime and blocking-I/O Facades, `include/kairo/comm/`, tutorials `07`–`08`, `12` | Period, worker lifecycle, backpressure, drop, or platform behavior changes |
| GPU | `zh/gpu/` | Public GPU API, tutorial `09`, hardware report | Backend, fallback, device, or performance conclusion changes |
| Advanced and reference | `zh/advanced/`, `zh/reference/` | Public headers, `docs/API.md`, design, tests | Stability boundary, execution path, migration policy changes |
| Site delivery | Theme, navigation, 404, deployment | `website/`, docs workflow, site checker | Route, base URL, dependency, or Pages flow changes |

## Definition of done for a content change

- Start from a user problem and state why to choose it, when not to choose it, and how failure is observed.
- Source code from a compiled example or provide an independent snippet-verification path.
- Do not overpromise priority, timeout, realtime behavior, or performance.
- Make a new entry reachable from its sidebar, parent page, and site search.
- Run the site build/link check; run relevant CTest when examples change.
- Update version scope and use the [release documentation checklist](https://github.com/Linductor-alkaid/kairo/blob/master/docs/RELEASE_CHECKLIST.md) when applicable.

## Content maturity

The site has full topic coverage and a buildable framework, but page presence does not prove deep user mastery. Future work prioritizes requirement judgment, end-to-end evolution, failure cases, capacity and stop strategy, reproducible performance conclusions, and platform/hardware differences.

<div class="maintenance-callout">
  <div>
    <strong>Found something unclear, unreproducible, or outdated?</strong>
    <p>Include the page URL, version, minimal reproduction, actual behavior, and expected behavior. For an integration scenario, also include task scale, thread model, and stop sequence.</p>
  </div>
  <a href="https://github.com/Linductor-alkaid/kairo/issues/new/choose">Report a documentation issue</a>
</div>
