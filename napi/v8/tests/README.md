# napi/v8 Tests

Tier-1 tests use GoogleTest and consume shared fixtures from `napi/tests`.
This CMake project only owns standalone `napi/v8` coverage. Runtime-owned
Node-API cases that depend on host event-loop or `libuv` behavior are exercised
from the Edge runtime build instead.

## Porting Rule

- Tests should be ported from Node as fully/verbatim as possible.
- Do not rewrite upstream test intent unless unavoidable.
- Adapt only execution glue (runner/shim/build wiring) as needed.
- If an upstream source path uses direct V8 APIs, replace those paths with
  N-API usage while preserving behavior.

## Current Ported Tests

- `2_function_arguments`
- `3_callbacks`

## Build And Run

The gtest binary requires a V8 library to link against.
Node-API fixtures without dedicated standalone runners are skipped even when
`NAPI_V8_BUILD_NODE_API_TESTS=ON`.

## V8 Build Modes

Configure V8 resolution with `NAPI_V8_BUILD_METHOD`:

- `prebuilt` (default): download pinned prebuilt V8 (`11.9.2`) for supported hosts.
- `source`: build V8 from `deps/v8` using `gn` + `ninja`.
- `local`: use local V8 include/library paths.

Compatibility override:

- `NAPI_V8_FORCE_LOCAL_BUILD=1` forces `local` mode.

Canonical variables:

- `NAPI_V8_INCLUDE_DIR`
- `NAPI_V8_LIBRARY`
- `NAPI_V8_EXTRA_LIBS`
- `NAPI_V8_DEFINES`

Deprecated aliases are still accepted (with warnings):

- `NAPI_V8_V8_INCLUDE_DIR`
- `NAPI_V8_V8_LIBRARY`
- `NAPI_V8_V8_MONOLITH_LIB`
- `NAPI_V8_V8_EXTRA_LIBS`
- `NAPI_V8_V8_DEFINES`

```bash
cmake -S napi/v8 -B napi/v8/build -DNAPI_V8_LIBRARY=/absolute/path/to/libv8_monolith.a
cmake --build napi/v8/build -j4
ctest --test-dir napi/v8/build --output-on-failure -R napi_v8_tier1_tests
```

If `NAPI_V8_LIBRARY` is not set, the core `napi_v8` library still builds
and the gtest executable is skipped.

### Default Prebuilt

```bash
cmake -S napi/v8 -B napi/v8/build
cmake --build napi/v8/build -j4
```

### Forced Local

```bash
NAPI_V8_FORCE_LOCAL_BUILD=1 cmake -S napi/v8 -B napi/v8/build
```

### Source Build

```bash
NAPI_V8_BUILD_METHOD=source cmake -S napi/v8 -B napi/v8/build
```

### Explicit Local Static/Dynamic

```bash
NAPI_V8_BUILD_METHOD=local \
NAPI_V8_INCLUDE_DIR=/absolute/path/to/include \
NAPI_V8_LIBRARY=/absolute/path/to/libv8.a \
cmake -S napi/v8 -B napi/v8/build
```

## Local Homebrew V8 Shortcut

Use the helper script for local testing with Homebrew V8 paths:

```bash
./napi/v8/scripts/test-local-v8.sh
```
