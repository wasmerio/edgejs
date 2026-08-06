# N-API Node Compatibility Known Issues

| | | Remarks |
| --- | --- | --- |
| **Status** | ▶️ | Registry for QuickJS N-API compatibility issues and internal subsystem notes. |
| **Severity** | Low | Documentation registry only; individual issue pages carry runtime severity. |

This directory records known Node-compatibility issues for the QuickJS N-API
backend. The issue pages are canonical; this index only carries status,
severity, and links.

| Status | Severity | Issue | Topic |
| --- | --- | --- | --- |
| 🟠 | Medium | [001_buffer.md](001_buffer.md) | Buffer |
| 🟠 | Medium | [002_console.md](002_console.md) | Console bootstrap binding |
| 🟢 | High | [003_contextify.md](003_contextify.md) | Contextify diagnostics |
| 🟠 | High | [004_environment.md](004_environment.md) | Environment lifecycle |
| 🟠 | Medium | [005_global_shims.md](005_global_shims.md) | Global shims |
| 🟢 | High | [006_microtasks.md](006_microtasks.md) | Promise hooks and microtasks |
| 🟠 | High | [007_module_loading.md](007_module_loading.md) | Module loading |
| 🟢 | Medium | [008_properties.md](008_properties.md) | Property setting semantics |
| 🟢 | Low | [009_quickjs_utilities.md](009_quickjs_utilities.md) | QuickJS utility ownership |
| 🟢 | Medium | [010_serdes.md](010_serdes.md) | Serialization and deserialization |
| ▶️ | Medium | [011_quickjs_wasix_atomics_patch.md](011_quickjs_wasix_atomics_patch.md) | QuickJS WASIX atomics guard patch |
| 🟠 | Medium | [013_v8_shaped_callsite_methods.md](013_v8_shaped_callsite_methods.md) | V8-shaped CallSite methods |
| 🟠 | Medium | [014_lifetime_tracing.md](014_lifetime_tracing.md) | QuickJS N-API lifetime tracing |
| ▶️ | High | [020_embedded_nul_module_source.md](020_embedded_nul_module_source.md) | Embedded NUL bytes in module source |
