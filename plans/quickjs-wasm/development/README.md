# Edge QuickJS Development Index

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Canonical entry point for QuickJS WASIX development notes and issue registries. |
| **Severity** | Low | Documentation structure only; runtime impact is tracked in the linked issue pages. |

This directory has two kinds of documentation:

- chronological notes that preserve development history;
- issue pages that are the canonical home for current status, severity, and
  known limitations.

Avoid restating issue details in index pages. Link to the issue page instead.

## Canonical Issue Registries

- [Troubleshooting registry](troubleshooting/README.md)
- [Node compatibility known issues](troubleshooting/node-compat/README.md)
- [Node test known issues](troubleshooting/node-test/README.md)
- [Astro SSR issues](troubleshooting/astro-ssr/)
- [Vite app issues](troubleshooting/vite-app/)
- [Next app issues](troubleshooting/next-app/)
- [Wasmer deploy issues](troubleshooting/wasmer-deploy/)

## Chronological Notes

These files are historical context. If a current problem is described here and
also has a troubleshooting page, treat the troubleshooting page as canonical.

| Note | Status | Scope |
| --- | --- | --- |
| [001_merge_analysis.md](001_merge_analysis.md) | 🟢 | Initial merge analysis and integration boundaries. |
| [002_native_bootstrap_contextify.md](002_native_bootstrap_contextify.md) | 🟢 | Native QuickJS bootstrap and contextify investigation history. |
| [003_repl_tty_readline.md](003_repl_tty_readline.md) | 🟢 | REPL TTY/readline investigation history. |
| [004_promise_hooks_microtasks.md](004_promise_hooks_microtasks.md) | 🟢 | Promise hooks and microtask investigation history. |
| [005_wasix_wasmer_http.md](005_wasix_wasmer_http.md) | 🟢 | WASIX/Wasmer HTTP bring-up history. |
| [006_framework_app_adapters.md](006_framework_app_adapters.md) | 🟠 | Framework adapter exploration history; current issues live under troubleshooting. |
| [007_framework_standalone_builds.md](007_framework_standalone_builds.md) | 🟠 | Standalone framework build findings; current issues live under troubleshooting. |
| [008_runtime_change_containment_rollback.md](008_runtime_change_containment_rollback.md) | 🟢 | Runtime containment and rollback history. |
| [009_node_test_failures_analysis.md](009_node_test_failures_analysis.md) | ▶️ | Node test failure clustering; per-problem pages live under `troubleshooting/node-test`. |
| [010_wasix_remaining_node_test_failures.md](010_wasix_remaining_node_test_failures.md) | ▶️ | Fix plan for the 12 in-process WASIX Node test failures after environment exclusions. |
| [011_tier2_standalone_build_tests.md](011_tier2_standalone_build_tests.md) | 🟢 | Tier 2 standalone build artifact harness, canary apps, and verification. |

## Development Task Notes

Development task directories preserve task-scoped context. They are not the
canonical home for known incompatibilities once a troubleshooting page exists.

| Directory | Status | Scope |
| --- | --- | --- |
| [dev_001_pr_cleanup_containment](dev_001_pr_cleanup_containment/) | 🟢 | Runtime cleanup containment history. |
| [dev_002_napi_promises_refactor](dev_002_napi_promises_refactor/) | 🟢 | QuickJS N-API internal refactor history. |
| [dev_003_quickjs_module_loading](dev_003_quickjs_module_loading/) | ▶️ | QuickJS `ModuleWrap` and Node JS loader interop plan for CommonJS/ESM parity. |
| [dev_004_v8_napi_lifetime_refactor](dev_004_v8_napi_lifetime_refactor/) | 🟢 | V8 N-API lifetime, handle-scope, reference, wrap, and finalizer refactor implemented and verified against shared V8/QuickJS N-API suites. |
| [dev_005_build_registry_modularization](dev_005_build_registry_modularization/) | 🟠 | Build modularization and internal binding registry implementation with provider-specific verification still pending. |
| [dev_006_pnpm_bundle_command](dev_006_pnpm_bundle_command/) | 🟠 | Bundled pnpm 10 command works under QuickJS WASIX with scoped filesystem adaptations; pnpm 11 and external command delegation remain follow-ups. |
| [dev_007_host_js_imported_napi](dev_007_host_js_imported_napi/) | 🟠 | Engine-free `syrusakbary/edgejs` package contract and artifact validation for a host-JavaScript N-API backend; host integration remains active. |
| [dev_008_wasix_ci_standalone_napi_cli](dev_008_wasix_ci_standalone_napi_cli/) | 🟢 | V8 WASIX tests use the locked standalone N-API CLI aligned with the current compatible Wasmer SDK branch. |
| [dev_009_napi_pr61_main_sync](dev_009_napi_pr61_main_sync/) | ▶️ | Sync N-API PR #61 with `main`, combine its Wasmer store-lending stack into SDK PR #6956, and advance EdgeJS PR #149 through green verification. |
| [dev_010_v8_1199_integration](dev_010_v8_1199_integration/) | 🟠 | Custom V8 build 11.9.9 integration passes the native Node lane; a pre-existing intermittent REPL CTest failure is documented. |

| [dev_010_quickjs_upstream_sync](dev_010_quickjs_upstream_sync/) | ▶️ | Synchronize the QuickJS fork with upstream master and validate N-API/EdgeJS integration PRs. |

## Status Icons

- `▶️`: open or active.
- `🟢`: resolved or stable.
- `🟠`: accepted with caveats, partial compatibility, or retained limitation.
- `🔴`: unresolved blocker.
