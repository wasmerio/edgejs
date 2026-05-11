# Production Readiness Smoke Checks

This document turns part of the 0.x production-readiness roadmap into concrete
smoke checks that can be run repeatedly before releases and after runtime
changes.

The goal is not to declare the runtime production-ready. These checks cover a
small native baseline for CLI startup, eval, built-in module loading, HTTP
server lifecycle, and repeated cold starts. Stress testing, low-resource
behavior, platform packaging, provider deployment, and broader WASIX validation
remain separate work.

## 0.x Readiness Matrix

| Area | Current validation | Status | Next evidence needed |
|---|---|---|---|
| Fast cold starts | Startup benchmarks and startup trace investigation | Partial | Release-gated cold-start thresholds per platform |
| Native runtime smoke | `make test-production-readiness` native smoke checks | Covered first pass | Keep the smoke path green on Linux and macOS |
| WASIX / safe mode smoke | Optional safe-mode smoke in `make test-production-readiness` | Partial | Required CI safe-mode pass for packaged WASIX artifacts |
| HTTPS / TLS in safe mode | Existing `make test-wasix-safe-mode` network/TLS smoke | Partial | Keep HTTPS smoke attached to WASIX package validation |
| Module loading | Smoke fixture validates common built-ins | Covered first pass | Add dependency-heavy package case |
| HTTP server lifecycle | Smoke fixture starts a loopback server and exits | Covered first pass | Add request-rate stress coverage |
| Stress testing | Not covered by this PR | Not covered | Many requests/second and long-running process checks |
| Low-resource behavior | Not covered by this PR | Not covered | Memory/CPU-constrained smoke runner |
| Stable unofficial N-API APIs | Existing N-API and internal binding tests | Partial | Public compatibility surface checklist |
| Platform binaries | CI produces Linux, macOS ARM, and WASIX artifacts | Partial | Windows, Android, iOS example app coverage |
| Production provider use | Roadmap target only | Not covered | Provider deployment report with blocker list |

## Smoke Harness

Run the native production-readiness smoke suite:

```bash
make test-production-readiness
```

By default this uses `./build-edge/edge`. Override the binary with:

```bash
EDGE_BINARY=/path/to/edge make test-production-readiness
```

The harness writes a Markdown summary and per-case logs to:

```text
artifacts/production-readiness/<timestamp>/
```

These generated artifacts are intentionally ignored by git. Copy relevant
summaries into issue or PR comments when they add context beyond the CI log.

Each case has a timeout so a stuck server lifecycle or runtime invocation fails
the smoke run instead of hanging indefinitely. Override it with:

```bash
EDGE_PRODUCTION_READINESS_CASE_TIMEOUT=60 make test-production-readiness
```

## Optional Safe-Mode Coverage

Safe mode requires Wasmer and a WASIX package. Enable the small no-network
safe-mode smoke set with:

```bash
EDGE_PRODUCTION_READINESS_SAFE=1 make test-production-readiness
```

Use a specific Wasmer binary or package with:

```bash
EDGE_PRODUCTION_READINESS_SAFE=1 \
WASMER_BIN=/path/to/wasmer \
EDGE_WASMER_PACKAGE=wasmer/edgejs@=<version> \
make test-production-readiness
```

For network/TLS safe-mode coverage, keep using:

```bash
make test-wasix-safe-mode
```

That path already covers HTTP, HTTPS, TLS, and related WASIX behavior through
the packaged runtime.

## Release Evidence Expectations

Before a 0.x production-readiness release, the project should be able to point
to public evidence for:

- native smoke results on supported desktop/server platforms
- WASIX safe-mode smoke results for the published package
- HTTPS/TLS safe-mode results
- startup benchmark trend
- at least one stress-test result
- low-resource behavior notes
- release artifact availability by platform
- an explicit unresolved-blocker list

This smoke path is only a starting point for the native checks in that broader
release-readiness picture.
