# Edge.js Architecture

`edge` is a runtime project that aims to replace Node.js while keeping N-API as
the core boundary. Unlike Node internals that integrate directly with V8 in many
paths, `edge` system bindings should be implemented through `napi/v8` APIs.

## Mission

- Build a Node-compatible runtime architecture centered on N-API contracts.
- Keep engine-specific details isolated behind `napi/v8`.
- Implement system/runtime bindings as N-API modules instead of direct V8 code.
- Advance in small, test-validated milestones.

## Porting Policy

- `edge` source and tests should be ported from Node as fully as possible.
- Preserve upstream structure and behavior semantics by default.
- Only exception: any source path using direct V8 APIs should be adapted to use
  N-API APIs instead.
- Prefer compatibility shims and adapter layers over rewriting upstream logic.
- Hard boundary: files under `src` must never include V8 headers
  (`v8.h`, `libplatform/libplatform.h`) or use `v8::` symbols.
- Host/bootstrap code that requires V8 must live outside `src` (for
  example, under `napi/v8`), while `src` remains N-API/Node-API only.

## Non-Goals (for early phases)

- Full Node parity in one step.
- Immediate support for every Node CLI/runtime flag.
- Rewriting all of Node internals at once.

## Core Architecture Direction

- **Runtime kernel**: process/bootstrap/module-loader/event-loop orchestration.
- **Binding layer**: system features exposed as N-API addons (backed by libuv,
  filesystem/network/process primitives).
- **Engine adapter**: `napi/v8` as the only JS engine integration surface.
- **Compatibility layer**: incremental behavior alignment with Node semantics.

## Roadmap Summary

Detailed milestones are tracked in the public roadmap issue:
<https://github.com/wasmerio/edgejs/issues/8>.

1. **Bootstrap**
   - `edge` executable that creates an environment through `napi/v8`.
   - Run/evaluate JS entry scripts.
2. **Minimal runtime primitives**
   - Implement foundational bindings (`process`, timers, console, basic module
     loading) through N-API.
3. **System binding expansion**
   - Add filesystem/path/os/crypto-like primitives as N-API-based modules.
4. **Node-compat iteration**
   - Port behavior test-by-test; close gaps in semantics and errors.
5. **Hardening and scale**
   - Stability, lifecycle, worker/thread integration, performance regression
     tracking.

## Testing Philosophy

Every roadmap step requires:

- Unit tests for new runtime/binding logic.
- Integration tests for end-to-end behavior from JS entrypoint.
- Compatibility tests aligned with Node expectations where feasible.
- A green gate before moving to the next milestone.

No phase should be marked complete without passing its defined test gate.

## WASIX Build

- Use `EDGE_NAPI_PROVIDER=imports` to compile `edge` with N-API imports only
  (no bundled `napi/v8` linkage).
- WASIX toolchain file: `wasix/wasix-toolchain.cmake`.
- Setup + build helper:
  - `wasix/setup-wasix-deps.sh`
  - `wasix/build-wasix.sh`
