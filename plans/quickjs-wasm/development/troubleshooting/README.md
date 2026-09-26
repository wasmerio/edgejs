# Edge QuickJS Troubleshooting Registry

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Canonical registry for QuickJS WASIX troubleshooting issue pages. |
| **Severity** | Low | Documentation registry only; individual issue pages carry runtime severity. |

Each problem should have one issue page. Keep diagnosis, status, severity, and
known limitations on that page. This registry should stay compact and avoid
duplicating issue details.

## Status Icons

- `▶️`: open or active.
- `🟢`: resolved or stable.
- `🟠`: accepted with caveats, partial compatibility, or retained limitation.
- `🔴`: unresolved blocker.

## Node Compatibility

| Status | Severity | Issue | Topic |
| --- | --- | --- | --- |
| 🟠 | Medium | [001_buffer.md](node-compat/napi/001_buffer.md) | Buffer |
| 🟠 | Medium | [002_console.md](node-compat/napi/002_console.md) | Console bootstrap binding |
| 🟢 | High | [003_contextify.md](node-compat/napi/003_contextify.md) | Contextify diagnostics |
| 🟠 | High | [004_environment.md](node-compat/napi/004_environment.md) | Environment lifecycle |
| 🟠 | Medium | [005_global_shims.md](node-compat/napi/005_global_shims.md) | Global shims |
| 🟢 | High | [006_microtasks.md](node-compat/napi/006_microtasks.md) | Promise hooks and microtasks |
| 🟠 | High | [007_module_loading.md](node-compat/napi/007_module_loading.md) | Module loading |
| 🟢 | Medium | [008_properties.md](node-compat/napi/008_properties.md) | Property setting semantics |
| 🟢 | Low | [009_quickjs_utilities.md](node-compat/napi/009_quickjs_utilities.md) | QuickJS utility ownership |
| 🟢 | Medium | [010_serdes.md](node-compat/napi/010_serdes.md) | Serialization and deserialization |
| ▶️ | Medium | [011_quickjs_wasix_atomics_patch.md](node-compat/napi/011_quickjs_wasix_atomics_patch.md) | QuickJS WASIX atomics guard patch |
| 🟠 | Medium | [013_v8_shaped_callsite_methods.md](node-compat/napi/013_v8_shaped_callsite_methods.md) | V8-shaped CallSite methods |
| 🟠 | Medium | [014_lifetime_tracing.md](node-compat/napi/014_lifetime_tracing.md) | QuickJS N-API lifetime tracing |
| 🟢 | High | [020_imported_napi_browser_pnpm_terminal_loss.md](node-compat/napi/020_imported_napi_browser_pnpm_terminal_loss.md) | Browser pnpm terminal loss |
| 🟠 | Medium | [003_minimal_intl_fallback.md](node-compat/edgejs/003_minimal_intl_fallback.md) | Minimal `Intl.DateTimeFormat` fallback |
| 🟠 | Medium | [004_native_inspector_stub.md](node-compat/edgejs/004_native_inspector_stub.md) | Native unavailable `inspector` stub |
| 🟠 | High | [010_stream_wrapper_unwrap_fallback.md](node-compat/edgejs/010_stream_wrapper_unwrap_fallback.md) | Stream wrapper unwrap fallback |
| 🟠 | Medium | [012_v8_ctest_environment_attach.md](node-compat/edgejs/012_v8_ctest_environment_attach.md) | V8 CTest runtime fixture environment attachment |
| ▶️ | High | [013_native_buffer_lease_regressions.md](node-compat/edgejs/013_native_buffer_lease_regressions.md) | Native buffer lease regressions |
| 🟠 | Medium | [016_napi_extern_wasix_linkage.md](node-compat/deploy/016_napi_extern_wasix_linkage.md) | `NAPI_EXTERN=` WASIX linkage rule |
| 🟠 | Low | [017_framework_static_server_adapters.md](node-compat/deploy/017_framework_static_server_adapters.md) | Framework static and ad hoc server adapters |
| 🟠 | Medium | [018_pnpm_deploy_graph_materialization.md](node-compat/deploy/018_pnpm_deploy_graph_materialization.md) | pnpm deploy graph materialization |
| 🟢 | High | [019_wasmer_napi_checkpoint_import.md](node-compat/deploy/019_wasmer_napi_checkpoint_import.md) | Wasmer N-API checkpoint import alignment |
| 🟢 | High | [020_wasix_ci_tail_failures.md](node-compat/deploy/020_wasix_ci_tail_failures.md) | WASIX CI tail failures |
| 🟢 | High | [021_wasix_ci_standalone_napi_cli.md](node-compat/deploy/021_wasix_ci_standalone_napi_cli.md) | WASIX CI standalone N-API CLI |
| 🟠 | High | [022_wasix_ci_tty_bridge_and_signal_pin.md](node-compat/deploy/022_wasix_ci_tty_bridge_and_signal_pin.md) | WASIX CI TTY bridge and signal pin |
| 🟢 | High | [023_wasix_standalone_nested_entry_paths.md](node-compat/deploy/023_wasix_standalone_nested_entry_paths.md) | WASIX standalone nested entry paths |

### 🟢 [024_gatsby_baseline_builds.md](node-compat/deploy/024_gatsby_baseline_builds.md): Gatsby baseline build failures

## Node Test

| Status | Severity | Issue | Topic |
| --- | --- | --- | --- |
| ▶️ | Medium | [001_buffer_limits_and_deprecations.md](node-test/001_buffer_limits_and_deprecations.md) | Buffer limits and deprecation parity |
| ▶️ | Medium | [002_console_inspect_and_stack_formatting.md](node-test/002_console_inspect_and_stack_formatting.md) | Console inspect and stack formatting |
| ▶️ | High | [003_node_test_public_api_exports.md](node-test/003_node_test_public_api_exports.md) | `node:test` public API exports |
| ▶️ | Medium | [004_diagnostics_channel_module_loader.md](node-test/004_diagnostics_channel_module_loader.md) | Diagnostics channel module loader events |
| ▶️ | Medium | [005_diagnostics_channel_async_context.md](node-test/005_diagnostics_channel_async_context.md) | Diagnostics channel async context |
| 🟢 | Low | [006_eventemitter_asyncresource_private_fields.md](node-test/006_eventemitter_asyncresource_private_fields.md) | EventEmitterAsyncResource private-field errors |
| ▶️ | High | [007_fetch_response_body_and_proxy_env.md](node-test/007_fetch_response_body_and_proxy_env.md) | Fetch Response body and HTTP proxy env |
| 🟠 | High | [008_https_proxy_tunnel_errors.md](node-test/008_https_proxy_tunnel_errors.md) | HTTPS proxy tunnel errors |
| ▶️ | Medium | [009_http_timers_and_header_limits.md](node-test/009_http_timers_and_header_limits.md) | HTTP timers and header limits |
| ▶️ | Low | [010_os_constants_and_userinfo.md](node-test/010_os_constants_and_userinfo.md) | OS constants and userInfo errors |
| ▶️ | Medium | [011_fastutf8stream_sync_wait.md](node-test/011_fastutf8stream_sync_wait.md) | FastUtf8Stream synchronous wait |
| ▶️ | High | [012_explicit_resource_management_syntax.md](node-test/012_explicit_resource_management_syntax.md) | Explicit resource management syntax |
| ▶️ | Medium | [013_stream_missing_builtins_and_async_iterators.md](node-test/013_stream_missing_builtins_and_async_iterators.md) | Stream missing builtins and async iterators |
| 🟢 | Medium | [014_string_decoder_utf8_boundaries.md](node-test/014_string_decoder_utf8_boundaries.md) | StringDecoder UTF-8 boundaries |
| ▶️ | Medium | [015_url_and_data_url_validation.md](node-test/015_url_and_data_url_validation.md) | URL and data URL validation |
| 🟢 | Medium | [016_whatwg_url_inspect_and_searchparams.md](node-test/016_whatwg_url_inspect_and_searchparams.md) | WHATWG URL inspect and search params |
| ▶️ | High | [017_http2_native_lifecycle_crashes.md](node-test/017_http2_native_lifecycle_crashes.md) | HTTP/2 native lifecycle crashes |
| 🟢 | High | [018_tls_securecontext_sni_lifetime.md](node-test/018_tls_securecontext_sni_lifetime.md) | TLS SecureContext, SNI, and setKeyCert lifetime |

## Astro SSR

| Status | Severity | Issue | Topic |
| --- | --- | --- | --- |
| ▶️ | High | [001_es_module_lexer_webassembly.md](astro-ssr/001_es_module_lexer_webassembly.md) | es-module-lexer WebAssembly import |
| 🟢 | High | [002_depd_callsite_methods.md](astro-ssr/002_depd_callsite_methods.md) | depd CallSite method compatibility |
| 🟠 | High | [003_cjs_reexport_named_exports.md](astro-ssr/003_cjs_reexport_named_exports.md) | CommonJS re-export named exports |
| 🟠 | Medium | [004_missing_intl.md](astro-ssr/004_missing_intl.md) | Missing Intl global |
| 🟠 | Low | [005_listen_eperm.md](astro-ssr/005_listen_eperm.md) | Listen EPERM on localhost |
| 🟢 | High | [006_floating_ui_utils_dom.md](astro-ssr/006_floating_ui_utils_dom.md) | Floating UI utils DOM subpath |
| 🟢 | High | [007_react_remove_scroll_bar_constants.md](astro-ssr/007_react_remove_scroll_bar_constants.md) | React Remove Scroll Bar constants subpath |
| 🟢 | High | [008_zustand_ind_create_export.md](astro-ssr/008_zustand_ind_create_export.md) | Zustand ind create export |
| 🟢 | High | [009_zustand_esm_default_export.md](astro-ssr/009_zustand_esm_default_export.md) | Zustand ESM default export |
| 🟢 | High | [010_use_gesture_controller_export.md](astro-ssr/010_use_gesture_controller_export.md) | Use Gesture Controller export |
| 🟢 | High | [011_route_stack_overflow.md](astro-ssr/011_route_stack_overflow.md) | Route stack overflow |
| 🟢 | High | [012_wasix_pnpm_symlink_resolution.md](astro-ssr/012_wasix_pnpm_symlink_resolution.md) | WASIX pnpm symlink resolution |
| 🟢 | High | [013_lucide_react_chevrondown_export.md](astro-ssr/013_lucide_react_chevrondown_export.md) | Lucide React ChevronDown export |
| 🟢 | Medium | [014_pnpm_deploy_externalized_runtime_links.md](astro-ssr/014_pnpm_deploy_externalized_runtime_links.md) | pnpm deploy externalized runtime links |

## Vite App

| Status | Severity | Issue | Topic |
| --- | --- | --- | --- |
| 🟠 | Low | [001_standalone_build.md](vite-app/001_standalone_build.md) | Standalone build findings |

## Next App

| Status | Severity | Issue | Topic |
| --- | --- | --- | --- |
| 🟢 | High | [001_standalone_v8_serdes.md](next-app/001_standalone_v8_serdes.md) | `require("v8")` / serdes findings |
| 🟢 | High | [002_standalone_inspector_stub.md](next-app/002_standalone_inspector_stub.md) | `require("inspector")` stub |
| ▶️ | High | [003_route_stack_exhausted.md](next-app/003_route_stack_exhausted.md) | Route request stack exhaustion |
| 🟢 | High | [004_next_config_swc_options_buffer.md](next-app/004_next_config_swc_options_buffer.md) | SWC options Buffer rejected by N-API |
| 🟢 | High | [005_entry_css_work_store_async_context.md](next-app/005_entry_css_work_store_async_context.md) | `entryCSSFiles` work store async context |

## Wasmer Deploy

| Status | Severity | Issue | Topic |
| --- | --- | --- | --- |
| 🟠 | High | [001_pnpm_directory_symlinks_webc.md](wasmer-deploy/001_pnpm_directory_symlinks_webc.md) | pnpm directory symlinks in WEBC package |
| 🟢 | High | [002_quickjs_wasix_napi_import_module_mismatch.md](wasmer-deploy/002_quickjs_wasix_napi_import_module_mismatch.md) | QuickJS WASIX N-API import module mismatch |
| 🟠 | High | [003_ci_safe_mode_missing_quickjs_artifact.md](wasmer-deploy/003_ci_safe_mode_missing_quickjs_artifact.md) | CI safe-mode missing QuickJS artifact |
| 🟢 | High | [004_wasix_safe_mode_https_exit.md](wasmer-deploy/004_wasix_safe_mode_https_exit.md) | WASIX safe-mode HTTPS exits before callbacks |
| 🟠 | High | [005_pnpm_cli_wasix_futimes.md](wasmer-deploy/005_pnpm_cli_wasix_futimes.md) | pnpm CLI WASIX filesystem fallbacks |

### ▶️ [021_quickjs_upstream_sync.md](node-compat/napi/021_quickjs_upstream_sync.md): QuickJS upstream synchronization compatibility

Overlapping coroutine GC, ArrayBuffer ownership, and lazy stack trace integration
while adopting upstream QuickJS-NG master; validation across all three repos.
