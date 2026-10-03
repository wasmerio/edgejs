# Pnpm Ci Failure

Status: 🟢 Implementation and existing-artifact verification complete (2026-10-02); root owns rebuilt-artifact verification.

## Scope

Investigate job 111074548689 of run 37078796961: bundled npm config get registry attempts to load /home/config. Record action plan before code changes.

## Dependencies and write ownership

Worker owns quickjs-wasm/pnpm/, scripts/test-wasix-pnpm.py, focused runtime fix if needed (coordinate first), and this note. No workflow/build-script edits. Root performs rebuilt verification. Other workers may be active in the shared checkout.

## Verification expectations

Reproduce exact failing command; compare V8 behavior; use LLDB for runtime failure as appropriate. Verify both npm/pnpm aliases and delegated config.

## Context

User requests latest main including submodules, both WASIX builds with latest wasixcc, CI fix, and release-please support. Baseline main: 9bff8bc6. Installed Wasmer: 7.4.2. Main package/version header is 0.0.0; registry manifests differ. Do not publish a real release as part of implementation.

## Action plan (2026-10-02)

1. Reproduce the exact CI command against the existing embedded QuickJS WASIX artifact and Wasmer 7.4.2.
2. Inspect pnpm's `runNpm()` delegation and isolate the subprocess argv behavior: `/bin/edge-npm-internal config get registry` starts Edge without the package command's `main-args`, making `config` the script path. Compare an explicit `/npm/bin/npm-cli.js` entrypoint through `/bin/edge`.
3. Keep the correction in the shared packaged pnpm launcher: for WASIX only, translate synchronous invocation of the private `/bin/edge-npm-internal` alias into `process.execPath /npm/bin/npm-cli.js <arguments>` before loading pnpm. Preserve caller cwd, env, stdio and other options; retain the public aliases and pinned upstream bundles.
4. Verify `.npmrc` delegation, versions, store path, online install, offline reuse and package scripts with the existing smoke suite. Root worker owns rebuilds and reruns this check against both new artifacts.
5. Update the existing pnpm troubleshooting note with the subprocess compatibility finding. No runtime C++ or Wasmer changes are required for this package-local adaptation.

## Reproduction

`python3 scripts/test-wasix-pnpm.py --package-dir quickjs-wasm --timeout 90` reproduces the exact CI error on Wasmer 7.4.2 with the September 30 existing artifact: `Cannot find module '/home/config'`.

A diagnostic `child_process.spawnSync()` confirms that the private alias receives only `config get registry`; the explicit interpreter + npm script form enters npm successfully. This is a WASIX package-command invocation failure, not a native runtime crash, so LLDB does not provide a useful reproducer. The native Node/V8 subprocess form already passes the script explicitly when invoking a JavaScript CLI through its interpreter.

## Implementation and verification

- Changed `quickjs-wasm/pnpm/edge-pnpm.cjs`, mounted by both WASIX package variants. It translates only the private npm fallback's synchronous subprocess target on WASIX, preserving args/options and all unrelated subprocess behavior.
- Updated existing [`005_pnpm_cli_wasix_futimes.md`](../troubleshooting/wasmer-deploy/005_pnpm_cli_wasix_futimes.md) instead of creating a duplicate troubleshooting issue.
- Existing `scripts/test-wasix-pnpm.py` already covers the exact failing delegated config command, so no redundant new test was added.
- Full `python3 scripts/test-wasix-pnpm.py --package-dir quickjs-wasm --timeout 90` passes with the existing September 30 embedded QuickJS artifact and Wasmer 7.4.2. Coverage includes both aliases' versions, custom project registry delegation, store paths, online React installation, offline frozen lockfile reuse, both aliases' package scripts and direct Edge `require('react')` resolution.
- `git diff --check` passes for the edited launcher and existing troubleshooting note.
- Root worker must rerun the pnpm smoke against the rebuilt QuickJS and imported-N-API artifacts; no builds or workflows were changed by this worker.

## Root integration verification

The complete bundled pnpm smoke passed against the clean rebuilt QuickJS artifact under Wasmer 7.4.2 on 2026-10-02, including delegated config, both aliases, online/offline installation, package scripts, and Edge module resolution. Log: `/private/tmp/edgejs-quickjs-pnpm.log`.
