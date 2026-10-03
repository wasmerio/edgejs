# Release Automation

Status: 🟢 Implemented and locally verified (2026-10-02); live publication awaits a merged release PR.

## Scope

Add release-please version PR, tag/GitHub release and assets for both backends; publish packages to wasmer.wtf and wasmer.io using existing secret names.

## Dependencies and write ownership

Worker owns release-please config/manifest, new release workflows/scripts/docs, src/edge_version.h release markers, and this note. Existing test workflows owned by root; coordinate changes. Preserve existing quickjs-wasm/wasmer.toml version 0.2.0 edit; do not modify it. Other workers may be active in the shared checkout.

## Verification expectations

Validate workflows and release config; ensure exact release commit/version, all six asset names, release builds without git suffix, registry publish gated on successful builds and secrets.

## Context

User requests latest main including submodules, both WASIX builds with latest wasixcc, CI fix, and release-please support. Baseline main: 9bff8bc6. Installed Wasmer: 7.4.2. Main package/version header is 0.0.0; registry manifests differ. Do not publish a real release as part of implementation.

## Implementation

- Added root simple Release Please configuration, manifest, `VERSION.txt`, and
  C++ block update markers. Unified baseline is `0.2.5`: read-only production
  registry GraphQL queries confirmed `wasmer/edgejs@0.2.5` and
  `wasmer/edgejs-quickjs@0.2.5` exist; staging standard `0.2.5` also exists.
  GitHub currently has no releases, so the config bootstraps from
  `9bff8bc6103622550c1099bae891713c8c370242`. Existing source manifest values
  stay intact, including the user's QuickJS `0.2.0`; the generated release PR
  will update both manifest versions.
- `.github/workflows/release.yml` validates its scripts and real Release Please
  updater behavior on PRs, creates a release PR on main pushes, then creates a
  tag and draft release after the PR is merged. Builds use the exact release
  commit via both existing reusable workflows (root owns their `workflow_call`
  changes and CMake/Makefile stable version plumbing).
- Six full build/test lanes must succeed before assets/publishing. Release
  outputs normalize package names to `wasmer/edgejs` and `wasmer/edgejs-quickjs`
  without changing development manifests. Checksums cover all six final ZIPs.
  Native Linux executables must report the release version, and Wasmer validates
  both package directories before upload. Publishing uses existing secret
  names with explicit matrix-to-secret mapping; empty staging credentials never
  fall back to production credentials.
- The GitHub release is published only after both registries succeed. Recovery
  accepts only an existing draft release tag and verifies its exact commit;
  completed releases cannot have assets clobbered. A fixed concurrency group
  serializes automatic and manual runs.
- Registry retries download any existing exact package version and compare all
  unpacked WEBC files plus parsed manifest against a local container. Identical
  publications are skipped; conflicting content fails without overwriting.
  No registry writes were executed during this task.
- Maintainer setup, release flow, six asset names, retry behavior, and local
  validation commands are in `RELEASING.md`.

## Verification

- `actionlint` on release and both reusable build/test workflows: passed.
- Nine Python release boundary tests: passed with Python 3.14.5. Ubuntu's Python
  meets the 3.11+ requirement; this host's default Python 3.9 does not.
- Release Please 17.3.0 schema validation and actual Generic/GenericToml
  updaters: passed. Header major/minor/patch and both package versions update;
  TOML comments and all other lines remain intact. This version matches the
  pinned `googleapis/release-please-action@v4.4.1` dependency.
- Built a fresh WEBC from the previously published standard `0.2.5` directory
  and compared its raw unpacked contents with the saved registry download:
  all 2,254 files and manifest match. This exercises the retry comparison with
  real WEBC data without publishing.
- `git diff --check`: passed.

## Remaining external validation

The first actual release PR/build/publication requires GitHub Actions and the
three existing secrets accessible to this workflow. This task does not create
a live tag, release, or registry version; merging a generated release PR is the
maintainer's publication step. Full local WASIX builds are tracked by root in
the sibling verification note.
