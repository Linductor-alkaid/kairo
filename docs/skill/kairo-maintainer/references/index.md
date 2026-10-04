# Executor Maintainer Knowledge Map

## Purpose

This skill gives an agent maintainer-level orientation without loading the entire API. It is deliberately a search map: an agent starts from intent, requirement, or implementation, loads one capability card, then verifies against code and tests.

## Design Decisions

| Decision | Rationale |
| --- | --- |
| Keep `SKILL.md` procedural and short | It is loaded first; detailed facts stay out of the default context. |
| Use three independent indexes | A feature request, a constraint, and a failing symbol are different starting points. |
| Store one capability per card | Updates stay local and a task does not pull unrelated APIs into context. |
| Link every card to headers, source, tests, and user docs | The map remains navigational; repository code remains authoritative. |
| Record observable semantics and invariants, not internals alone | Refactors may move internals while acceptance behavior must remain stable. |

## Retrieval Contract

1. Start with one of `by-business-feature.md`, `by-requirement.md`, or `by-implementation.md`.
2. Read the selected card before opening broad source trees.
3. Use `rg` on the card's symbols and paths to locate current code; do not assume a listed line number remains current.
4. Validate behavior using the named test family and the user-visible result/status path.

## Card Schema

Every capability card contains:

- **Use it when**: search terms and a decision boundary.
- **Public boundary**: stable headers and principal symbols, not an exhaustive reference.
- **Implementation trail**: the likely owner and execution path.
- **Observable contract**: completion, rejection, error, timeout, and lifecycle semantics.
- **Change safeguards**: invariants and validation targets.
- **Related material**: user guide and design references.

## Coverage And Update Policy

The initial cards cover the maintained execution domains, not every overload. Add a separate card when a feature gains its own lifecycle, ownership model, failure mode, or test family. Otherwise extend the closest card with a narrow entry.

In the same pull request as a behavior change:

1. Update the relevant card if the public boundary, implementation trail, invariant, failure semantics, or test target changed.
2. Add terms to every applicable index so the feature can be found from business, requirement, and implementation language.
3. Check links and run the card's targeted tests. Do not claim a performance property without a reproducible measurement.
