# Make This Skill Available To A Downstream AI

An AI started in an application repository cannot automatically discover a skill kept in Executor's source tree. Use one of these explicit paths.

## Read It In Place

When Executor is cloned, vendored, or checked out beside the application, give the agent this instruction:

```text
Read /path/to/executor/docs/skill/executor-integration/SKILL.md before integrating Executor.
```

Use the actual checked-out path. This is the lowest-cost option and keeps the agent on the exact library revision in use.

## Copy It With The Application

Copy the complete `docs/skill/executor-integration/` directory into a documented location in the application repository, then put its exact `SKILL.md` path in that project's agent instruction or task prompt. Keep the `references/` directory beside it; the entry file links to it.

Refresh the copy whenever Executor is upgraded. Do not copy only `SKILL.md`, because scenario cards are loaded on demand.

## Select The Right Audience

`executor-integration` is for using the library in an application. `executor-maintainer` is separate material for contributors changing Executor's own implementation; do not load it for normal integration work.
