# edge Test Strategy

This file defines how to keep roadmap execution safe and measurable.

## Porting Compliance Checks

Every milestone should verify:

- Source and tests remain aligned with upstream Node where available.
- Any deviations are explicitly justified and tracked.
- The only intended code-level deviation is replacing direct V8 API usage with
  N-API usage.

## Test Layers

- **Unit tests**
  - Scope: internal runtime helpers, binding argument validation, lifecycle
    state transitions.
  - Goal: fast feedback for local iteration.

- **Integration tests**
  - Scope: run scripts against `edge` and verify observable behavior.
  - Goal: confirm wiring between runtime kernel + N-API bindings.

- **Compatibility tests**
  - Scope: selected Node test ports and behavior parity checks.
  - Goal: quantify how close `edge` is to Node semantics.

- **Stress/lifecycle tests**
  - Scope: repeated startup/shutdown, GC-triggered finalization, async teardown.
  - Goal: catch crashes, leaks, and ordering bugs.

## Required Coverage by Phase

- Phase 0-1: unit + smoke integration mandatory.
- Phase 2-3: unit + integration + compatibility mandatory.
- Phase 4-5: all layers mandatory, including stress and regression checks.

## CI Gate Policy

Every change should satisfy:

1. Affected unit tests pass.
2. Affected integration tests pass.
3. Compatibility subset for touched area passes.
4. No new flaky tests introduced.
5. Porting compliance checks pass for touched files.

## Flakiness Policy

- Any flaky test blocks phase completion.
- Mark flaky tests with issue owner and root-cause note immediately.
- Phase gate remains red until deterministic behavior is restored.

## Reporting

Each milestone should publish:

- pass/fail counts by test layer
- known failures (with owners)
- risk notes for uncovered areas

