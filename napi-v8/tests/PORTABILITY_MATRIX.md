# N-API Test Portability Matrix

This matrix classifies Node test directories for Phase 1 `napi-v8`.

- `implement_now`: feasible in standalone `napi-v8` with current scope.
- `defer_phase2`: depends on Node runtime/libuv/event-loop/lifecycle hooks.
- `out_of_scope_phase1`: tightly coupled to Node process/runtime semantics.
- `in_progress`: currently ported and wired.

## `js-native-api` (`node/test/js-native-api`)

### in_progress

- `2_function_arguments` (ported to gtest harness)
- `3_callbacks` (ported to gtest harness)
- `4_object_factory` (ported to gtest harness)
- `5_function_factory` (ported to gtest harness)
- `6_object_wrap` (ported to gtest harness)
- `7_factory_wrap` (ported to gtest harness)
- `8_passing_wrapped` (ported to gtest harness)
- `test_array` (ported to gtest harness)
- `test_constructor` (ported to gtest harness)
- `test_error` (ported to gtest harness)
- `test_exception` (ported to gtest harness)
- `test_function` (ported to gtest harness)
- `test_number` (ported to gtest harness)
- `test_new_target` (ported to gtest harness)
- `test_reference` (ported to gtest harness)
- `test_string` (ported to gtest harness)
- `test_symbol` (ported to gtest harness)
- `test_conversions` (ported to gtest harness)
- `test_properties` (ported to gtest harness)
- `test_general` (ported to gtest harness)
- `test_object` (ported to gtest harness)
- `test_bigint` (ported to gtest harness)
- `test_date` (ported to gtest harness)
- `test_dataview` (ported to gtest harness)
- `test_sharedarraybuffer` (ported to gtest harness)
- `test_typedarray` (ported to gtest harness)
- `test_promise` (ported to gtest harness)
- `test_handle_scope` (ported to gtest harness)
- `test_reference_double_free` (ported to gtest harness)

### implement_now

- (none currently)

### defer_phase2

- `test_cannot_run_js`
- `test_finalizer`
- `test_instance_data`

## `node-api` (`node/test/node-api`)

### in_progress

- `test_general` (ported to gtest harness)
- `test_exception` (ported to gtest harness)
- `test_instance_data` (ported core addon + `test_ref_then_set` + `test_set_then_ref`)

### defer_phase2

- `1_hello_world`
- `test_async`
- `test_async_cleanup_hook`
- `test_async_context`
- `test_buffer`
- `test_callback_scope`
- `test_cleanup_hook`
- `test_env_teardown_gc`
- `test_fatal`
- `test_fatal_exception`
- `test_init_order`
- `test_make_callback`
- `test_make_callback_recurse`
- `test_reference_by_node_api_version`
- `test_threadsafe_function`
- `test_threadsafe_function_shutdown`
- `test_uv_loop`
- `test_uv_threadpool_size`
- `test_worker_buffer_callback`
- `test_worker_terminate`
- `test_worker_terminate_finalization`

### out_of_scope_phase1

- `test_null_init` (module init edge-case semantics tied to Node loader)
- `test_sea_addon` (SEA-specific behavior)
