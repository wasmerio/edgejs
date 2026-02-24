# node-napi

`node-napi` aims to implement Node.js fully on top of N-API, directly, instead of depending on V8.

## Why This Project

Node.js is deeply tied to V8 internals. This project explores a different architecture: a Node runtime centered on N-API contracts, where engine-specific behavior is isolated behind stable interfaces.

The long-term intent is to make runtime internals less engine-coupled while preserving compatibility expectations for native modules.

## Project Direction

- Keep N-API as the primary abstraction boundary.
- Avoid exposing V8-specific details from public or internal integration layers.
- Progress incrementally, validating behavior with tests at every phase.

## Current Status

The project is in planning/bootstrap stage. The roadmap below defines the execution order and milestones.

## Roadmap

### Phase 1: `napi-v8` Compatibility Layer

Create a `napi-v8` project that exposes N-API using V8 under the hood, without exposing V8 details.

Scope:
- Base the implementation on Node's N-API implementation:
  - `node_api.h`
  - `js_native_api_v8.h`
  - `js_native_api_v8.cc`
  - `js_native_api_v8_internals.h`
  - and the files they depend on
- Preserve N-API behavior and contracts while hiding V8 internals.

Exit criteria:
- A standalone layer that passes initial N-API behavior checks.
- No V8-specific API leakage through the public N-API surface.

### Phase 2: Base `node-napi` Runtime

Create a base `node-napi` project using the same dependencies as Node, except for V8-related dependencies.

Scope:
- Reproduce Node's non-V8 dependency stack as closely as possible.
- Establish the runtime skeleton required to host N-API-first execution.

Exit criteria:
- Buildable base project with Node-aligned non-V8 dependencies.
- Clear separation boundaries for engine-related components.

### Phase 3: Native Modules + Test Convergence

Implement native modules with N-API and get all tests passing one by one.

Scope:
- Port/enable modules incrementally through N-API interfaces.
- Run the test suite continuously and resolve failures iteratively.

Exit criteria:
- Native modules functioning through N-API.
- Progressive test pass-rate growth toward full green.

## Notes

- Initial implementation references Node's existing N-API files and their dependencies.
- This README focuses on goals and direction; implementation docs can be added as the codebase grows.
