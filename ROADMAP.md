# Edge.js Roadmap (Test-Gated)

This roadmap is execution-oriented: each step has deliverables, tests, and exit
criteria.

## Global Porting Constraint

- Port source and tests from Node as fully and verbatim as possible.
- Keep divergence minimal and documented.
- The only intended source adaptation is replacing direct V8 API usage with
  N-API usage.
- Hard enforcement: `src` must not include V8 headers (`v8.h`,
  `libplatform/libplatform.h`) and must not use `v8::` symbols.
- Any required V8 host/bootstrap code must live outside `src`.

## Phase 0 - Project Bootstrap

### Deliverables
- Root directory structure and runtime docs.
- Build target for a minimal executable.
- Wiring to consume `napi/v8` as dependency.

### Tests
- Build test: project config + link succeeds.
- Smoke test: execute a script that prints and exits.

### Exit Criteria
- `edge` can run `hello.js` and returns exit code `0`.
- Bootstrap smoke tests are automated.

---

## Phase 1 - JS Runtime Entry + Script Execution

### Deliverables
- Script execution pipeline (`edge script.js`).
- Error propagation and exit-code mapping.
- Minimal globals (`global`, `console`, safe startup surface).

### Tests
- Unit: argument parsing and exit-code mapping.
- Integration: valid script, thrown error, syntax error.
- Regression: deterministic behavior across repeated runs.

### Exit Criteria
- Stable script execution and predictable failure behavior.
- All Phase 1 tests green.

---

## Phase 2 - N-API First System Bindings

### Deliverables
- Core runtime bindings exposed via N-API modules (no direct V8 in binding
  implementations).
- Initial modules:
  - timer/event-loop primitives
  - process basics
  - module resolution/loading primitives

### Tests
- Unit: each binding function validates inputs/outputs/errors.
- Integration: JS-level behavior tests calling bindings.
- Negative tests: invalid args, missing modules, lifecycle edge cases.

### Exit Criteria
- Core bindings used by runtime pass all unit/integration tests.
- Direct V8 use in system bindings is blocked by code review policy.
- Any V8-dependent upstream source path is replaced by N-API equivalents.

---

## Phase 3 - Node Compatibility Core

### Deliverables
- Compatibility-focused behavior for common Node patterns.
- Progressive support for selected Node test subsets.

### Tests
- Ported compatibility suites (selected `node/test` cases).
- Golden-output tests for CLI/runtime behavior.
- Backward-compat checks for errors/messages where required.

### Exit Criteria
- Agreed compatibility baseline is green and reproducible.
- Known incompatibilities documented with owners.

---

## Phase 4 - System Module Expansion

### Deliverables
- Expand built-in modules through N-API-backed implementations.
- Add worker/event-loop lifecycle paths and teardown guarantees.

### Tests
- Module-level unit tests.
- Cross-module integration tests.
- Stress tests (repeated load/unload, GC/lifecycle pressure, async teardown).

### Exit Criteria
- No known flaky tests in target module set.
- Stress suite passes in CI and local runs.

---

## Phase 5 - Hardening + Release Readiness

### Deliverables
- Performance baseline and regression detection.
- Crash/fatal-path test coverage.
- Packaging and developer workflow docs.

### Tests
- Performance regression suite.
- Long-running stability tests.
- End-to-end CLI tests.

### Exit Criteria
- Release checklist fully green.
- Reproducible CI pipeline with stable pass rate.

---

## Execution Model

For every phase and milestone:

1. Implement the smallest vertical slice.
2. Add/expand tests first-class (unit + integration).
3. Run full relevant suite locally.
4. Merge only if milestone gate is green.
5. Track remaining known gaps explicitly.

## Suggested Test Gates (per PR)

- **Gate A**: New/changed unit tests pass.
- **Gate B**: Affected integration tests pass.
- **Gate C**: Phase compatibility subset passes.
- **Gate D**: No newly introduced flaky/fatal regressions.
