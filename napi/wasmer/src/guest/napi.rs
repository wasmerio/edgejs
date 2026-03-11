// ============================================================
// WASM import handlers for "napi" module
// ============================================================

// --- Init ---

use std::ffi::CString;

use wasmer::{Function, FunctionEnv, FunctionEnvMut, Imports, Store};

use crate::{RuntimeEnv, guest::{MAX_GUEST_CSTRING_SCAN, callback::CB_ENV_PTR}, snapi::*};

use super::util::*;

fn guest_napi_wasm_init_env(_env: FunctionEnvMut<RuntimeEnv>) -> i32 {
    let ok = unsafe { snapi_bridge_init() };
    if ok != 0 {
        1
    } else {
        0
    }
}

fn guest_unofficial_napi_create_env(
    mut env: FunctionEnvMut<RuntimeEnv>,
    module_api_version: i32,
    env_out_ptr: i32,
    scope_out_ptr: i32,
) -> i32 {
    let mut env_handle: u32 = 0;
    let mut scope_handle: u32 = 0;
    let status = unsafe {
        snapi_bridge_unofficial_create_env(module_api_version, &mut env_handle, &mut scope_handle)
    };
    if status != 0 {
        return status;
    }
    if env_out_ptr > 0 {
        write_guest_u32(&mut env, env_out_ptr as u32, env_handle);
    }
    if scope_out_ptr > 0 {
        write_guest_u32(&mut env, scope_out_ptr as u32, scope_handle);
    }
    0
}

fn guest_unofficial_napi_release_env(_env: FunctionEnvMut<RuntimeEnv>, scope_ptr: i32) -> i32 {
    let scope_handle = if scope_ptr > 0 { scope_ptr as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_release_env(scope_handle) }
}

fn guest_unofficial_napi_process_microtasks(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_process_microtasks(env_handle) }
}

fn guest_unofficial_napi_request_gc_for_testing(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_request_gc_for_testing(env_handle) }
}

fn guest_unofficial_napi_get_promise_details(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    promise: i32,
    state_ptr: i32,
    result_ptr: i32,
    has_result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let promise_id = if promise > 0 { promise as u32 } else { 0 };
    let mut state = 0i32;
    let mut result_id = 0u32;
    let mut has_result = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_get_promise_details(
            env_handle,
            promise_id,
            &mut state,
            &mut result_id,
            &mut has_result,
        )
    };
    if status != 0 {
        return status;
    }
    if state_ptr > 0 {
        write_guest_i32(&mut env, state_ptr as u32, state);
    }
    if result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    if has_result_ptr > 0 {
        write_guest_u8(&mut env, has_result_ptr as u32, (has_result != 0) as u8);
    }
    0
}

fn guest_unofficial_napi_get_proxy_details(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    proxy: i32,
    target_ptr: i32,
    handler_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let proxy_id = if proxy > 0 { proxy as u32 } else { 0 };
    let mut target_id = 0u32;
    let mut handler_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_get_proxy_details(
            env_handle,
            proxy_id,
            &mut target_id,
            &mut handler_id,
        )
    };
    if status != 0 {
        return status;
    }
    if target_ptr > 0 {
        write_guest_u32(&mut env, target_ptr as u32, target_id);
    }
    if handler_ptr > 0 {
        write_guest_u32(&mut env, handler_ptr as u32, handler_id);
    }
    0
}

fn guest_unofficial_napi_preview_entries(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    value: i32,
    entries_ptr: i32,
    is_key_value_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let value_id = if value > 0 { value as u32 } else { 0 };
    let mut entries_id = 0u32;
    let mut is_key_value = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_preview_entries(
            env_handle,
            value_id,
            &mut entries_id,
            &mut is_key_value,
        )
    };
    if status != 0 {
        return status;
    }
    if entries_ptr > 0 {
        write_guest_u32(&mut env, entries_ptr as u32, entries_id);
    }
    if is_key_value_ptr > 0 {
        write_guest_u8(&mut env, is_key_value_ptr as u32, (is_key_value != 0) as u8);
    }
    0
}

fn guest_unofficial_napi_get_call_sites(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    frames: i32,
    callsites_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut callsites_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_get_call_sites(env_handle, frames as u32, &mut callsites_id)
    };
    if status == 0 && callsites_ptr > 0 {
        write_guest_u32(&mut env, callsites_ptr as u32, callsites_id);
    }
    status
}

fn guest_unofficial_napi_get_caller_location(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    location_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut location_id = 0u32;
    let status =
        unsafe { snapi_bridge_unofficial_get_caller_location(env_handle, &mut location_id) };
    if status == 0 && location_ptr > 0 {
        write_guest_u32(&mut env, location_ptr as u32, location_id);
    }
    status
}

fn guest_unofficial_napi_arraybuffer_view_has_buffer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    value: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let value_id = if value > 0 { value as u32 } else { 0 };
    let mut result = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_arraybuffer_view_has_buffer(env_handle, value_id, &mut result)
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u8(&mut env, result_ptr as u32, (result != 0) as u8);
    }
    status
}

fn guest_unofficial_napi_get_constructor_name(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    value: i32,
    name_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let value_id = if value > 0 { value as u32 } else { 0 };
    let mut name_id = 0u32;
    let status =
        unsafe { snapi_bridge_unofficial_get_constructor_name(env_handle, value_id, &mut name_id) };
    if status == 0 && name_ptr > 0 {
        write_guest_u32(&mut env, name_ptr as u32, name_id);
    }
    status
}

fn guest_unofficial_napi_create_private_symbol(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    desc_ptr: i32,
    length: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let wl = length as u32;
    let desc = if desc_ptr > 0 {
        if wl == 0xFFFFFFFFu32 {
            read_guest_c_string(&mut env, desc_ptr)
        } else {
            read_guest_bytes(&mut env, desc_ptr, wl as usize)
        }
    } else {
        Some(Vec::new())
    };
    let Some(desc) = desc else {
        return 1;
    };
    let cs = CString::new(desc).unwrap_or_default();
    let mut out = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_create_private_symbol(env_handle, cs.as_ptr(), wl, &mut out)
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, out);
    }
    status
}

fn guest_unofficial_napi_get_continuation_preserved_embedder_data(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut out = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_get_continuation_preserved_embedder_data(env_handle, &mut out)
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, out);
    }
    status
}

fn guest_unofficial_napi_set_continuation_preserved_embedder_data(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    value: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let value_id = if value > 0 { value as u32 } else { 0 };
    unsafe {
        snapi_bridge_unofficial_set_continuation_preserved_embedder_data(env_handle, value_id)
    }
}

fn guest_unofficial_napi_notify_datetime_configuration_change(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_notify_datetime_configuration_change(env_handle) }
}

fn guest_unofficial_napi_set_enqueue_foreground_task_callback(
    _env: FunctionEnvMut<RuntimeEnv>,
    _napi_env: i32,
    _callback: i32,
    _target: i32,
) -> i32 {
    // The Wasmer harness drives execution on a single host thread and does not
    // currently bridge this callback to the native platform queue.
    // Return napi_ok so modules that import this symbol can instantiate.
    0
}

fn guest_unofficial_napi_set_fatal_error_callbacks(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    fatal_callback: i32,
    oom_callback: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let fatal_id = if fatal_callback > 0 {
        fatal_callback as u32
    } else {
        0
    };
    let oom_id = if oom_callback > 0 {
        oom_callback as u32
    } else {
        0
    };
    unsafe { snapi_bridge_unofficial_set_fatal_error_callbacks(env_handle, fatal_id, oom_id) }
}

fn guest_unofficial_napi_terminate_execution(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_terminate_execution(env_handle) }
}

fn guest_unofficial_napi_structured_clone(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    value: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let value_id = if value > 0 { value as u32 } else { 0 };
    let mut out = 0u32;
    let status =
        unsafe { snapi_bridge_unofficial_structured_clone(env_handle, value_id, &mut out) };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, out);
    }
    status
}

fn guest_unofficial_napi_serialize_value(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _napi_env: i32,
    value: i32,
    payload_out_ptr: i32,
) -> i32 {
    if payload_out_ptr > 0 {
        write_guest_u32(&mut env, payload_out_ptr as u32, value.max(0) as u32);
    }
    0
}

fn guest_unofficial_napi_deserialize_value(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _napi_env: i32,
    payload: i32,
    result_out_ptr: i32,
) -> i32 {
    if result_out_ptr > 0 {
        write_guest_u32(&mut env, result_out_ptr as u32, payload.max(0) as u32);
    }
    0
}

fn guest_unofficial_napi_release_serialized_value(_env: FunctionEnvMut<RuntimeEnv>, _payload: i32) {
}

fn guest_unofficial_napi_enqueue_microtask(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    callback: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let callback_id = if callback > 0 { callback as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_enqueue_microtask(env_handle, callback_id) }
}

fn guest_unofficial_napi_set_promise_reject_callback(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    callback: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let callback_id = if callback > 0 { callback as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_set_promise_reject_callback(env_handle, callback_id) }
}

fn guest_unofficial_napi_get_own_non_index_properties(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    value: i32,
    filter_bits: i32,
    result_out_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let value_id = if value > 0 { value as u32 } else { 0 };
    let filter = if filter_bits > 0 {
        filter_bits as u32
    } else {
        0
    };
    let mut out = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_get_own_non_index_properties(env_handle, value_id, filter, &mut out)
    };
    if status == 0 && result_out_ptr > 0 {
        write_guest_u32(&mut env, result_out_ptr as u32, out);
    }
    status
}

fn guest_unofficial_napi_get_process_memory_info(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    heap_total_out: i32,
    heap_used_out: i32,
    external_out: i32,
    array_buffers_out: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut heap_total = 0.0f64;
    let mut heap_used = 0.0f64;
    let mut external = 0.0f64;
    let mut array_buffers = 0.0f64;
    let status = unsafe {
        snapi_bridge_unofficial_get_process_memory_info(
            env_handle,
            &mut heap_total,
            &mut heap_used,
            &mut external,
            &mut array_buffers,
        )
    };
    if status != 0 {
        return status;
    }
    if heap_total_out > 0 {
        write_guest_f64(&mut env, heap_total_out as u32, heap_total);
    }
    if heap_used_out > 0 {
        write_guest_f64(&mut env, heap_used_out as u32, heap_used);
    }
    if external_out > 0 {
        write_guest_f64(&mut env, external_out as u32, external);
    }
    if array_buffers_out > 0 {
        write_guest_f64(&mut env, array_buffers_out as u32, array_buffers);
    }
    0
}

fn guest_unofficial_napi_create_serdes_binding(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut out = 0u32;
    let status = unsafe { snapi_bridge_unofficial_create_serdes_binding(env_handle, &mut out) };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, out);
    }
    status
}

fn guest_napi_add_env_cleanup_hook(
    _env: FunctionEnvMut<RuntimeEnv>,
    _napi_env: i32,
    _fun: i32,
    _arg: i32,
) -> i32 {
    0
}

fn guest_napi_remove_env_cleanup_hook(
    _env: FunctionEnvMut<RuntimeEnv>,
    _napi_env: i32,
    _fun: i32,
    _arg: i32,
) -> i32 {
    0
}

fn guest_unofficial_napi_contextify_contains_module_syntax(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    code: i32,
    filename: i32,
    resource_name_or_undefined: i32,
    cjs_var_in_scope: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let code_id = if code > 0 { code as u32 } else { 0 };
    let filename_id = if filename > 0 { filename as u32 } else { 0 };
    let resource_name_id = if resource_name_or_undefined > 0 {
        resource_name_or_undefined as u32
    } else {
        0
    };
    let mut result = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_contextify_contains_module_syntax(
            env_handle,
            code_id,
            filename_id,
            resource_name_id,
            cjs_var_in_scope,
            &mut result,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u8(&mut env, result_ptr as u32, (result != 0) as u8);
    }
    status
}

fn guest_unofficial_napi_contextify_make_context(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    sandbox_or_symbol: i32,
    name: i32,
    origin_or_undefined: i32,
    allow_code_gen_strings: i32,
    allow_code_gen_wasm: i32,
    own_microtask_queue: i32,
    host_defined_option_id: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_contextify_make_context(
            env_handle,
            sandbox_or_symbol as u32,
            name as u32,
            if origin_or_undefined > 0 {
                origin_or_undefined as u32
            } else {
                0
            },
            allow_code_gen_strings,
            allow_code_gen_wasm,
            own_microtask_queue,
            if host_defined_option_id > 0 {
                host_defined_option_id as u32
            } else {
                0
            },
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

#[allow(clippy::too_many_arguments)]
fn guest_unofficial_napi_contextify_run_script(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    sandbox_or_null: i32,
    source: i32,
    filename: i32,
    line_offset: i32,
    column_offset: i32,
    timeout: i64,
    display_errors: i32,
    break_on_sigint: i32,
    break_on_first_line: i32,
    host_defined_option_id: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_contextify_run_script(
            env_handle,
            if sandbox_or_null > 0 {
                sandbox_or_null as u32
            } else {
                0
            },
            source as u32,
            filename as u32,
            line_offset,
            column_offset,
            timeout,
            display_errors,
            break_on_sigint,
            break_on_first_line,
            if host_defined_option_id > 0 {
                host_defined_option_id as u32
            } else {
                0
            },
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_contextify_dispose_context(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    sandbox_or_context_global: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe {
        snapi_bridge_unofficial_contextify_dispose_context(
            env_handle,
            sandbox_or_context_global as u32,
        )
    }
}

#[allow(clippy::too_many_arguments)]
fn guest_unofficial_napi_contextify_compile_function(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    code: i32,
    filename: i32,
    line_offset: i32,
    column_offset: i32,
    cached_data_or_undefined: i32,
    produce_cached_data: i32,
    parsing_context_or_undefined: i32,
    context_extensions_or_undefined: i32,
    params_or_undefined: i32,
    host_defined_option_id: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_contextify_compile_function(
            env_handle,
            code as u32,
            filename as u32,
            line_offset,
            column_offset,
            if cached_data_or_undefined > 0 {
                cached_data_or_undefined as u32
            } else {
                0
            },
            produce_cached_data,
            if parsing_context_or_undefined > 0 {
                parsing_context_or_undefined as u32
            } else {
                0
            },
            if context_extensions_or_undefined > 0 {
                context_extensions_or_undefined as u32
            } else {
                0
            },
            if params_or_undefined > 0 {
                params_or_undefined as u32
            } else {
                0
            },
            if host_defined_option_id > 0 {
                host_defined_option_id as u32
            } else {
                0
            },
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_contextify_compile_function_for_cjs_loader(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    code: i32,
    filename: i32,
    is_sea_main: i32,
    should_detect_module: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_contextify_compile_function_for_cjs_loader(
            env_handle,
            code as u32,
            filename as u32,
            is_sea_main,
            should_detect_module,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_contextify_create_cached_data(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    code: i32,
    filename: i32,
    line_offset: i32,
    column_offset: i32,
    host_defined_option_id: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_contextify_create_cached_data(
            env_handle,
            code as u32,
            filename as u32,
            line_offset,
            column_offset,
            if host_defined_option_id > 0 {
                host_defined_option_id as u32
            } else {
                0
            },
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

#[allow(clippy::too_many_arguments)]
fn guest_unofficial_napi_module_wrap_create_source_text(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    wrapper: i32,
    url: i32,
    context_or_undefined: i32,
    source: i32,
    line_offset: i32,
    column_offset: i32,
    cached_data_or_id: i32,
    handle_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut handle_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_create_source_text(
            env_handle,
            wrapper as u32,
            url as u32,
            if context_or_undefined > 0 {
                context_or_undefined as u32
            } else {
                0
            },
            source as u32,
            line_offset,
            column_offset,
            if cached_data_or_id > 0 {
                cached_data_or_id as u32
            } else {
                0
            },
            &mut handle_id,
        )
    };
    if status == 0 && handle_ptr > 0 {
        write_guest_u32(&mut env, handle_ptr as u32, handle_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_create_synthetic(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    wrapper: i32,
    url: i32,
    context_or_undefined: i32,
    export_names: i32,
    synthetic_eval_steps: i32,
    handle_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut handle_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_create_synthetic(
            env_handle,
            wrapper as u32,
            url as u32,
            if context_or_undefined > 0 {
                context_or_undefined as u32
            } else {
                0
            },
            export_names as u32,
            synthetic_eval_steps as u32,
            &mut handle_id,
        )
    };
    if status == 0 && handle_ptr > 0 {
        write_guest_u32(&mut env, handle_ptr as u32, handle_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_destroy(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_module_wrap_destroy(env_handle, handle as u32) }
}

fn guest_unofficial_napi_module_wrap_get_module_requests(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_get_module_requests(
            env_handle,
            handle as u32,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_link(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    count: i32,
    linked_handles_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let count_u = count as u32;
    let linked_handles = if count_u > 0 {
        let Some(ids) = read_guest_u32_array(&mut env, linked_handles_ptr, count_u as usize) else {
            return 1;
        };
        ids
    } else {
        Vec::new()
    };
    unsafe {
        snapi_bridge_unofficial_module_wrap_link(
            env_handle,
            handle as u32,
            count_u,
            linked_handles.as_ptr(),
        )
    }
}

fn guest_unofficial_napi_module_wrap_instantiate(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe { snapi_bridge_unofficial_module_wrap_instantiate(env_handle, handle as u32) }
}

fn guest_unofficial_napi_module_wrap_evaluate(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    timeout: i64,
    break_on_sigint: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_evaluate(
            env_handle,
            handle as u32,
            timeout,
            break_on_sigint,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_evaluate_sync(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_evaluate_sync(env_handle, handle as u32, &mut result_id)
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_get_namespace(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_get_namespace(env_handle, handle as u32, &mut result_id)
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_get_status(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    status_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut status_val = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_get_status(env_handle, handle as u32, &mut status_val)
    };
    if status == 0 && status_ptr > 0 {
        write_guest_i32(&mut env, status_ptr as u32, status_val);
    }
    status
}

fn guest_unofficial_napi_module_wrap_get_error(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_get_error(env_handle, handle as u32, &mut result_id)
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_has_top_level_await(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_has_top_level_await(
            env_handle,
            handle as u32,
            &mut result,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u8(&mut env, result_ptr as u32, (result != 0) as u8);
    }
    status
}

fn guest_unofficial_napi_module_wrap_has_async_graph(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result = 0i32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_has_async_graph(env_handle, handle as u32, &mut result)
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u8(&mut env, result_ptr as u32, (result != 0) as u8);
    }
    status
}

fn guest_unofficial_napi_module_wrap_set_export(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    export_name: i32,
    export_value: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe {
        snapi_bridge_unofficial_module_wrap_set_export(
            env_handle,
            handle as u32,
            export_name as u32,
            if export_value > 0 {
                export_value as u32
            } else {
                0
            },
        )
    }
}

fn guest_unofficial_napi_module_wrap_set_module_source_object(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    source_object: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe {
        snapi_bridge_unofficial_module_wrap_set_module_source_object(
            env_handle,
            handle as u32,
            if source_object > 0 {
                source_object as u32
            } else {
                0
            },
        )
    }
}

fn guest_unofficial_napi_module_wrap_get_module_source_object(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_get_module_source_object(
            env_handle,
            handle as u32,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_create_cached_data(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_create_cached_data(
            env_handle,
            handle as u32,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_set_import_module_dynamically_callback(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    callback: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe {
        snapi_bridge_unofficial_module_wrap_set_import_module_dynamically_callback(
            env_handle,
            if callback > 0 { callback as u32 } else { 0 },
        )
    }
}

fn guest_unofficial_napi_module_wrap_set_initialize_import_meta_object_callback(
    _env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    callback: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    unsafe {
        snapi_bridge_unofficial_module_wrap_set_initialize_import_meta_object_callback(
            env_handle,
            if callback > 0 { callback as u32 } else { 0 },
        )
    }
}

fn guest_unofficial_napi_module_wrap_import_module_dynamically(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    argc: i32,
    argv_ptr: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let argc_u = argc as u32;
    let argv_ids = if argc_u > 0 {
        let Some(ids) = read_guest_u32_array(&mut env, argv_ptr, argc_u as usize) else {
            return 1;
        };
        ids
    } else {
        Vec::new()
    };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_import_module_dynamically(
            env_handle,
            argc_u,
            argv_ids.as_ptr(),
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

fn guest_unofficial_napi_module_wrap_create_required_module_facade(
    mut env: FunctionEnvMut<RuntimeEnv>,
    napi_env: i32,
    handle: i32,
    result_ptr: i32,
) -> i32 {
    let env_handle = if napi_env > 0 { napi_env as u32 } else { 0 };
    let mut result_id = 0u32;
    let status = unsafe {
        snapi_bridge_unofficial_module_wrap_create_required_module_facade(
            env_handle,
            handle as u32,
            &mut result_id,
        )
    };
    if status == 0 && result_ptr > 0 {
        write_guest_u32(&mut env, result_ptr as u32, result_id);
    }
    status
}

// --- Singleton getters ---

fn guest_napi_get_undefined(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_undefined(&mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_null(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_null(&mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_boolean(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    value: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_boolean(value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_global(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_global(&mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// --- Value creation ---

fn guest_napi_create_string_utf8(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    str_ptr: i32,
    length: i32,
    rp: i32,
) -> i32 {
    let wl = length as u32;
    let sb = if wl == 0xFFFFFFFFu32 {
        read_guest_c_string(&mut env, str_ptr)
    } else {
        read_guest_bytes(&mut env, str_ptr, wl as usize)
    };
    let Some(sb) = sb else {
        return 1;
    };
    let cs = CString::new(sb).unwrap_or_default();
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_string_utf8(cs.as_ptr(), wl, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_string_latin1(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    str_ptr: i32,
    length: i32,
    rp: i32,
) -> i32 {
    let wl = length as u32;
    let sb = if wl == 0xFFFFFFFFu32 {
        read_guest_c_string(&mut env, str_ptr)
    } else {
        read_guest_bytes(&mut env, str_ptr, wl as usize)
    };
    let Some(sb) = sb else {
        return 1;
    };
    let cs = CString::new(sb).unwrap_or_default();
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_string_latin1(cs.as_ptr(), wl, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_int32(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    value: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_int32(value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_uint32(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    value: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_uint32(value as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_double(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    value: f64,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_double(value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_int64(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    value: i64,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_int64(value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_object(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_object(&mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_array(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_array(&mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_array_with_length(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    length: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_array_with_length(length as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_symbol(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    desc: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_symbol(desc as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_error(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    code: i32,
    msg: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_error(code as u32, msg as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_type_error(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    code: i32,
    msg: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_type_error(code as u32, msg as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_range_error(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    code: i32,
    msg: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_range_error(code as u32, msg as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_bigint_int64(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    value: i64,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_bigint_int64(value, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_bigint_uint64(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    value: i64,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_bigint_uint64(value as u64, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_date(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, time: f64, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_date(time, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_external(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    data: i32,
    _finalize_cb: i32,
    _finalize_hint: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_external(data as u64, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// --- Value reading ---

fn guest_napi_get_value_string_utf8(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    bp: i32,
    bs: i32,
    rp: i32,
) -> i32 {
    let hbs = if bs <= 0 { 0usize } else { bs as usize };
    let mut hb = vec![0i8; hbs];
    let mut rl: usize = 0;
    let s = unsafe {
        snapi_bridge_get_value_string_utf8(
            vh as u32,
            if hbs > 0 {
                hb.as_mut_ptr()
            } else {
                std::ptr::null_mut()
            },
            hbs,
            &mut rl,
        )
    };
    if s != 0 {
        return s;
    }
    if bp > 0 && hbs > 0 {
        let n = hbs.min(rl + 1);
        let b: Vec<u8> = hb[..n].iter().map(|&x| x as u8).collect();
        write_guest_bytes(&mut env, bp as u32, &b);
    }
    if rp > 0 {
        write_guest_u32(&mut env, rp as u32, rl as u32);
    }
    0
}

fn guest_napi_get_value_string_latin1(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    bp: i32,
    bs: i32,
    rp: i32,
) -> i32 {
    let hbs = if bs <= 0 { 0usize } else { bs as usize };
    let mut hb = vec![0i8; hbs];
    let mut rl: usize = 0;
    let s = unsafe {
        snapi_bridge_get_value_string_latin1(
            vh as u32,
            if hbs > 0 {
                hb.as_mut_ptr()
            } else {
                std::ptr::null_mut()
            },
            hbs,
            &mut rl,
        )
    };
    if s != 0 {
        return s;
    }
    if bp > 0 && hbs > 0 {
        let n = hbs.min(rl + 1);
        let b: Vec<u8> = hb[..n].iter().map(|&x| x as u8).collect();
        write_guest_bytes(&mut env, bp as u32, &b);
    }
    if rp > 0 {
        write_guest_u32(&mut env, rp as u32, rl as u32);
    }
    0
}

fn guest_napi_get_value_int32(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_int32(vh as u32, &mut r) };
    if s == 0 {
        write_guest_i32(&mut env, rp as u32, r);
    }
    s
}

fn guest_napi_get_value_uint32(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    rp: i32,
) -> i32 {
    let mut r: u32 = 0;
    let s = unsafe { snapi_bridge_get_value_uint32(vh as u32, &mut r) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, r);
    }
    s
}

fn guest_napi_get_value_double(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    rp: i32,
) -> i32 {
    let mut r: f64 = 0.0;
    let s = unsafe { snapi_bridge_get_value_double(vh as u32, &mut r) };
    if s == 0 {
        write_guest_f64(&mut env, rp as u32, r);
    }
    s
}

fn guest_napi_get_value_int64(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    rp: i32,
) -> i32 {
    let mut r: i64 = 0;
    let s = unsafe { snapi_bridge_get_value_int64(vh as u32, &mut r) };
    if s == 0 {
        write_guest_i64(&mut env, rp as u32, r);
    }
    s
}

fn guest_napi_get_value_bool(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_bool(vh as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_napi_get_value_bigint_int64(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    vp: i32,
    lp: i32,
) -> i32 {
    let mut val: i64 = 0;
    let mut lossless: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_bigint_int64(vh as u32, &mut val, &mut lossless) };
    if s == 0 {
        write_guest_i64(&mut env, vp as u32, val);
        write_guest_u8(&mut env, lp as u32, lossless as u8);
    }
    s
}

fn guest_napi_get_value_bigint_uint64(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    vp: i32,
    lp: i32,
) -> i32 {
    let mut val: u64 = 0;
    let mut lossless: i32 = 0;
    let s = unsafe { snapi_bridge_get_value_bigint_uint64(vh as u32, &mut val, &mut lossless) };
    if s == 0 {
        write_guest_u64(&mut env, vp as u32, val);
        write_guest_u8(&mut env, lp as u32, lossless as u8);
    }
    s
}

fn guest_napi_get_date_value(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    rp: i32,
) -> i32 {
    let mut r: f64 = 0.0;
    let s = unsafe { snapi_bridge_get_date_value(vh as u32, &mut r) };
    if s == 0 {
        write_guest_f64(&mut env, rp as u32, r);
    }
    s
}

fn guest_napi_get_value_external(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    rp: i32,
) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_get_value_external(vh as u32, &mut data) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, data as u32);
    }
    s
}

// --- Type checking ---

fn guest_napi_typeof(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_typeof(vh as u32, &mut r) };
    if s == 0 {
        write_guest_i32(&mut env, rp as u32, r);
    }
    s
}

macro_rules! guest_is_check {
    ($name:ident, $bridge:ident) => {
        fn $name(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
            let mut r: i32 = 0;
            let s = unsafe { $bridge(vh as u32, &mut r) };
            if s == 0 {
                write_guest_u8(&mut env, rp as u32, r as u8);
            }
            s
        }
    };
}

guest_is_check!(guest_napi_is_array, snapi_bridge_is_array);
guest_is_check!(guest_napi_is_error, snapi_bridge_is_error);
guest_is_check!(guest_napi_is_arraybuffer, snapi_bridge_is_arraybuffer);
guest_is_check!(guest_napi_is_typedarray, snapi_bridge_is_typedarray);
guest_is_check!(guest_napi_is_dataview, snapi_bridge_is_dataview);
guest_is_check!(guest_napi_is_date, snapi_bridge_is_date);
guest_is_check!(guest_napi_is_promise, snapi_bridge_is_promise);

fn guest_napi_instanceof(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    obj: i32,
    ctor: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_instanceof(obj as u32, ctor as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

// --- Coercion ---

macro_rules! guest_coerce {
    ($name:ident, $bridge:ident) => {
        fn $name(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32, rp: i32) -> i32 {
            let mut out: u32 = 0;
            let s = unsafe { $bridge(vh as u32, &mut out) };
            if s == 0 {
                write_guest_u32(&mut env, rp as u32, out);
            }
            s
        }
    };
}

guest_coerce!(guest_napi_coerce_to_bool, snapi_bridge_coerce_to_bool);
guest_coerce!(guest_napi_coerce_to_number, snapi_bridge_coerce_to_number);
guest_coerce!(guest_napi_coerce_to_string, snapi_bridge_coerce_to_string);
guest_coerce!(guest_napi_coerce_to_object, snapi_bridge_coerce_to_object);

// --- Object operations ---

fn guest_napi_set_property(
    _env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    k: i32,
    v: i32,
) -> i32 {
    unsafe { snapi_bridge_set_property(o as u32, k as u32, v as u32) }
}

fn guest_napi_get_property(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    k: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_property(o as u32, k as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_has_property(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    k: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_has_property(o as u32, k as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_napi_has_own_property(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    k: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_has_own_property(o as u32, k as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_napi_delete_property(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    k: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_delete_property(o as u32, k as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_napi_set_named_property(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    np: i32,
    v: i32,
) -> i32 {
    let Some(nb) = read_guest_c_string(&mut env, np) else {
        return 1;
    };
    let cn = CString::new(nb).unwrap_or_default();
    unsafe { snapi_bridge_set_named_property(o as u32, cn.as_ptr(), v as u32) }
}

fn guest_napi_get_named_property(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    np: i32,
    rp: i32,
) -> i32 {
    let Some(nb) = read_guest_c_string(&mut env, np) else {
        return 1;
    };
    let cn = CString::new(nb).unwrap_or_default();
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_named_property(o as u32, cn.as_ptr(), &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_has_named_property(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    np: i32,
    rp: i32,
) -> i32 {
    let Some(nb) = read_guest_c_string(&mut env, np) else {
        return 1;
    };
    let cn = CString::new(nb).unwrap_or_default();
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_has_named_property(o as u32, cn.as_ptr(), &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_napi_set_element(
    _env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    idx: i32,
    v: i32,
) -> i32 {
    unsafe { snapi_bridge_set_element(o as u32, idx as u32, v as u32) }
}

fn guest_napi_get_element(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    idx: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_element(o as u32, idx as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_has_element(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    idx: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_has_element(o as u32, idx as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_napi_delete_element(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    idx: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_delete_element(o as u32, idx as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_napi_get_array_length(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    ah: i32,
    rp: i32,
) -> i32 {
    let mut r: u32 = 0;
    let s = unsafe { snapi_bridge_get_array_length(ah as u32, &mut r) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, r);
    }
    s
}

fn guest_napi_get_property_names(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_property_names(o as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_all_property_names(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    o: i32,
    mode: i32,
    filter: i32,
    conversion: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_get_all_property_names(o as u32, mode, filter, conversion, &mut out)
    };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_prototype(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32, rp: i32) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_prototype(o as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_object_freeze(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32) -> i32 {
    unsafe { snapi_bridge_object_freeze(o as u32) }
}

fn guest_napi_object_seal(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, o: i32) -> i32 {
    unsafe { snapi_bridge_object_seal(o as u32) }
}

// --- Comparison ---

fn guest_napi_strict_equals(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    lhs: i32,
    rhs: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_strict_equals(lhs as u32, rhs as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

// --- Error handling ---

fn guest_napi_throw(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, err: i32) -> i32 {
    unsafe { snapi_bridge_throw(err as u32) }
}

fn guest_napi_throw_error(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    code_ptr: i32,
    msg_ptr: i32,
) -> i32 {
    let code_bytes = if code_ptr != 0 {
        read_guest_c_string(&mut env, code_ptr)
    } else {
        None
    };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else {
        return 1;
    };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe {
        snapi_bridge_throw_error(
            c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            c_msg.as_ptr(),
        )
    }
}

fn guest_napi_throw_type_error(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    code_ptr: i32,
    msg_ptr: i32,
) -> i32 {
    let code_bytes = if code_ptr != 0 {
        read_guest_c_string(&mut env, code_ptr)
    } else {
        None
    };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else {
        return 1;
    };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe {
        snapi_bridge_throw_type_error(
            c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            c_msg.as_ptr(),
        )
    }
}

fn guest_napi_throw_range_error(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    code_ptr: i32,
    msg_ptr: i32,
) -> i32 {
    let code_bytes = if code_ptr != 0 {
        read_guest_c_string(&mut env, code_ptr)
    } else {
        None
    };
    let Some(msg_bytes) = read_guest_c_string(&mut env, msg_ptr) else {
        return 1;
    };
    let c_code = code_bytes.map(|b| CString::new(b).unwrap_or_default());
    let c_msg = CString::new(msg_bytes).unwrap_or_default();
    unsafe {
        snapi_bridge_throw_range_error(
            c_code.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            c_msg.as_ptr(),
        )
    }
}

fn guest_napi_is_exception_pending(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_is_exception_pending(&mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_napi_get_and_clear_last_exception(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_and_clear_last_exception(&mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// --- Promise ---

fn guest_napi_create_promise(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    dp: i32,
    rp: i32,
) -> i32 {
    let mut deferred_id: u32 = 0;
    let mut promise_id: u32 = 0;
    let s = unsafe { snapi_bridge_create_promise(&mut deferred_id, &mut promise_id) };
    if s == 0 {
        write_guest_u32(&mut env, dp as u32, deferred_id);
        write_guest_u32(&mut env, rp as u32, promise_id);
    }
    s
}

fn guest_napi_resolve_deferred(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, d: i32, v: i32) -> i32 {
    unsafe { snapi_bridge_resolve_deferred(d as u32, v as u32) }
}

fn guest_napi_reject_deferred(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, d: i32, v: i32) -> i32 {
    unsafe { snapi_bridge_reject_deferred(d as u32, v as u32) }
}

// --- ArrayBuffer ---

fn guest_napi_create_arraybuffer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    byte_length: i32,
    data_ptr: i32,
    rp: i32,
) -> i32 {
    // Try to create a guest-memory-backed ArrayBuffer (for WASIX)
    let malloc_fn = env.data().malloc_fn.clone();
    let memory = env.data().memory.clone();

    if let (Some(malloc_fn), Some(memory)) = (malloc_fn, memory) {
        // Allocate memory in the guest's linear memory
        let guest_ptr: i32 = {
            let (_, mut store_ref) = env.data_and_store_mut();
            match malloc_fn.call(&mut store_ref, byte_length) {
                Ok(ptr) if ptr > 0 => ptr,
                _ => return 1, // allocation failed
            }
        };

        // Get host pointer corresponding to the guest allocation
        let host_addr: u64 = {
            let (_, store_ref) = env.data_and_store_mut();
            let view = memory.view(&store_ref);
            let host_base = view.data_ptr() as u64;
            host_base + guest_ptr as u64
        };

        // Zero-initialize the guest memory region
        {
            let zeros = vec![0u8; byte_length as usize];
            write_guest_bytes(&mut env, guest_ptr as u32, &zeros);
        }

        // Create external arraybuffer backed by guest memory
        let mut out: u32 = 0;
        let s = unsafe {
            snapi_bridge_create_external_arraybuffer(host_addr, byte_length as u32, &mut out)
        };
        if s == 0 {
            // Store mapping from handle ID → guest data pointer (V8 sandbox remaps external pointers)
            env.data_mut().guest_data_ptrs.insert(out, guest_ptr as u32);
            write_guest_u32(&mut env, rp as u32, out);
            if data_ptr > 0 {
                write_guest_u32(&mut env, data_ptr as u32, guest_ptr as u32);
            }
        }
        s
    } else {
        // Fallback: host-memory-backed arraybuffer (non-WASIX path)
        let mut out: u32 = 0;
        let s = unsafe { snapi_bridge_create_arraybuffer(byte_length as u32, &mut out) };
        if s == 0 {
            write_guest_u32(&mut env, rp as u32, out);
        }
        s
    }
}

fn guest_napi_create_external_arraybuffer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    external_data: i32,
    byte_length: i32,
    _finalize_cb: i32,
    _finalize_hint: i32,
    rp: i32,
) -> i32 {
    let memory = env.data().memory.clone();
    let Some(memory) = memory else {
        return 1;
    };

    let host_addr: u64 = {
        let (_, store_ref) = env.data_and_store_mut();
        let view = memory.view(&store_ref);
        let host_base = view.data_ptr() as u64;
        host_base + external_data as u64
    };

    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_create_external_arraybuffer(host_addr, byte_length as u32, &mut out)
    };
    if s == 0 {
        env.data_mut()
            .guest_data_ptrs
            .insert(out, external_data as u32);
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_create_external_buffer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    external_data: i32,
    byte_length: i32,
    _finalize_cb: i32,
    _finalize_hint: i32,
    rp: i32,
) -> i32 {
    let memory = env.data().memory.clone();
    let Some(memory) = memory else {
        return 1;
    };

    let host_addr: u64 = {
        let (_, store_ref) = env.data_and_store_mut();
        let view = memory.view(&store_ref);
        let host_base = view.data_ptr() as u64;
        host_base + external_data as u64
    };

    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_external_buffer(host_addr, byte_length as u32, &mut out) };
    if s == 0 {
        env.data_mut()
            .guest_data_ptrs
            .insert(out, external_data as u32);
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_arraybuffer_info(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    data_ptr: i32,
    len_ptr: i32,
) -> i32 {
    let mut host_data_addr: u64 = 0;
    let mut bl: u32 = 0;
    let s = unsafe { snapi_bridge_get_arraybuffer_info(vh as u32, &mut host_data_addr, &mut bl) };
    if s != 0 {
        return s;
    }

    if len_ptr > 0 {
        write_guest_u32(&mut env, len_ptr as u32, bl);
    }

    if data_ptr > 0 {
        if let Some(guest_data_ptr) =
            resolve_or_copy_host_data_to_guest(&mut env, vh as u32, host_data_addr, bl as usize)
        {
            write_guest_u32(&mut env, data_ptr as u32, guest_data_ptr);
        }
    }
    0
}

fn guest_napi_detach_arraybuffer(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, vh: i32) -> i32 {
    unsafe { snapi_bridge_detach_arraybuffer(vh as u32) }
}

fn guest_napi_is_detached_arraybuffer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    rp: i32,
) -> i32 {
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_is_detached_arraybuffer(vh as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_node_api_is_sharedarraybuffer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    value: i32,
    rp: i32,
) -> i32 {
    let mut r = 0i32;
    let s = unsafe { snapi_bridge_is_sharedarraybuffer(value as u32, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

fn guest_node_api_create_sharedarraybuffer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    byte_length: i32,
    data_ptr: i32,
    rp: i32,
) -> i32 {
    let mut host_data_addr = 0u64;
    let mut out = 0u32;
    let s = unsafe {
        snapi_bridge_create_sharedarraybuffer(byte_length as u32, &mut host_data_addr, &mut out)
    };
    if s != 0 {
        return s;
    }

    if data_ptr > 0 {
        write_guest_u32(&mut env, data_ptr as u32, host_data_addr as u32);
    }
    write_guest_u32(&mut env, rp as u32, out);
    s
}

fn guest_node_api_set_prototype(
    _env: FunctionEnvMut<RuntimeEnv>,
    _napi_env: i32,
    object: i32,
    prototype: i32,
) -> i32 {
    let object_id = if object > 0 { object as u32 } else { 0 };
    let prototype_id = if prototype > 0 { prototype as u32 } else { 0 };
    unsafe { snapi_bridge_node_api_set_prototype(object_id, prototype_id) }
}

// --- TypedArray ---

fn guest_napi_create_typedarray(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    typ: i32,
    length: i32,
    ab: i32,
    offset: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_create_typedarray(typ, length as u32, ab as u32, offset as u32, &mut out)
    };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_typedarray_info(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    tp: i32,
    lp: i32,
    dp: i32,
    abp: i32,
    bop: i32,
) -> i32 {
    let mut typ: i32 = 0;
    let mut len: u32 = 0;
    let mut host_data_addr: u64 = 0;
    let mut ab: u32 = 0;
    let mut bo: u32 = 0;
    let s = unsafe {
        snapi_bridge_get_typedarray_info(
            vh as u32,
            &mut typ,
            &mut len,
            &mut host_data_addr,
            &mut ab,
            &mut bo,
        )
    };
    if s == 0 {
        if tp > 0 {
            write_guest_i32(&mut env, tp as u32, typ);
        }
        if lp > 0 {
            write_guest_u32(&mut env, lp as u32, len);
        }
        if dp > 0 {
            let elem_size = match typ {
                0 | 1 | 2 => 1usize,
                3 | 4 | 13 | 14 => 2usize,
                5 | 6 | 15 | 16 => 4usize,
                7 | 8 | 9 | 10 | 11 | 12 | 17 | 18 => 8usize,
                _ => 1usize,
            };
            let byte_len = len as usize * elem_size;
            if let Some(guest_data_ptr) =
                resolve_or_copy_host_data_to_guest(&mut env, vh as u32, host_data_addr, byte_len)
            {
                write_guest_u32(&mut env, dp as u32, guest_data_ptr);
            }
        }
        if abp > 0 {
            write_guest_u32(&mut env, abp as u32, ab);
        }
        if bop > 0 {
            write_guest_u32(&mut env, bop as u32, bo);
        }
    }
    s
}

// --- DataView ---

fn guest_napi_create_dataview(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    bl: i32,
    ab: i32,
    bo: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_dataview(bl as u32, ab as u32, bo as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_dataview_info(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    blp: i32,
    dp: i32,
    abp: i32,
    bop: i32,
) -> i32 {
    let mut bl: u32 = 0;
    let mut host_data_addr: u64 = 0;
    let mut ab: u32 = 0;
    let mut bo: u32 = 0;
    let s = unsafe {
        snapi_bridge_get_dataview_info(vh as u32, &mut bl, &mut host_data_addr, &mut ab, &mut bo)
    };
    if s == 0 {
        if blp > 0 {
            write_guest_u32(&mut env, blp as u32, bl);
        }
        if dp > 0 {
            if let Some(guest_data_ptr) =
                resolve_or_copy_host_data_to_guest(&mut env, vh as u32, host_data_addr, bl as usize)
            {
                write_guest_u32(&mut env, dp as u32, guest_data_ptr);
            }
        }
        if abp > 0 {
            write_guest_u32(&mut env, abp as u32, ab);
        }
        if bop > 0 {
            write_guest_u32(&mut env, bop as u32, bo);
        }
    }
    s
}

// --- References ---

fn guest_napi_create_reference(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    irc: i32,
    rp: i32,
) -> i32 {
    let mut ref_id: u32 = 0;
    let s = unsafe { snapi_bridge_create_reference(vh as u32, irc as u32, &mut ref_id) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, ref_id);
    }
    s
}

fn guest_napi_delete_reference(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, r: i32) -> i32 {
    unsafe { snapi_bridge_delete_reference(r as u32) }
}

fn guest_napi_reference_ref(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, r: i32, rp: i32) -> i32 {
    let mut count: u32 = 0;
    let s = unsafe { snapi_bridge_reference_ref(r as u32, &mut count) };
    if s == 0 && rp > 0 {
        write_guest_u32(&mut env, rp as u32, count);
    }
    s
}

fn guest_napi_reference_unref(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    r: i32,
    rp: i32,
) -> i32 {
    let mut count: u32 = 0;
    let s = unsafe { snapi_bridge_reference_unref(r as u32, &mut count) };
    if s == 0 && rp > 0 {
        write_guest_u32(&mut env, rp as u32, count);
    }
    s
}

fn guest_napi_get_reference_value(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    r: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_reference_value(r as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// --- Handle scopes ---

fn guest_napi_open_handle_scope(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, _rp: i32) -> i32 {
    0
}
fn guest_napi_close_handle_scope(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, _scope: i32) -> i32 {
    0
}

fn guest_napi_open_escapable_handle_scope(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    rp: i32,
) -> i32 {
    let mut scope_id: u32 = 0;
    let s = unsafe { snapi_bridge_open_escapable_handle_scope(&mut scope_id) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, scope_id);
    }
    s
}

fn guest_napi_close_escapable_handle_scope(
    _env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    scope: i32,
) -> i32 {
    unsafe { snapi_bridge_close_escapable_handle_scope(scope as u32) }
}

fn guest_napi_escape_handle(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    scope: i32,
    escapee: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_escape_handle(scope as u32, escapee as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// --- Type tagging ---

fn guest_napi_type_tag_object(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    obj: i32,
    tag_ptr: i32,
) -> i32 {
    // napi_type_tag is { uint64_t lower; uint64_t upper; } = 16 bytes
    let Some(tag_bytes) = read_guest_bytes(&mut env, tag_ptr, 16) else {
        return 1;
    };
    let lower = u64::from_le_bytes(tag_bytes[0..8].try_into().unwrap());
    let upper = u64::from_le_bytes(tag_bytes[8..16].try_into().unwrap());
    unsafe { snapi_bridge_type_tag_object(obj as u32, lower, upper) }
}

fn guest_napi_check_object_type_tag(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    obj: i32,
    tag_ptr: i32,
    rp: i32,
) -> i32 {
    let Some(tag_bytes) = read_guest_bytes(&mut env, tag_ptr, 16) else {
        return 1;
    };
    let lower = u64::from_le_bytes(tag_bytes[0..8].try_into().unwrap());
    let upper = u64::from_le_bytes(tag_bytes[8..16].try_into().unwrap());
    let mut r: i32 = 0;
    let s = unsafe { snapi_bridge_check_object_type_tag(obj as u32, lower, upper, &mut r) };
    if s == 0 {
        write_guest_u8(&mut env, rp as u32, r as u8);
    }
    s
}

// --- Function calling ---

fn guest_napi_call_function(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    recv: i32,
    func: i32,
    argc: i32,
    argv_ptr: i32,
    rp: i32,
) -> i32 {
    let argc_u = argc as u32;
    let argv_ids = if argc_u > 0 {
        let Some(ids) = read_guest_u32_array(&mut env, argv_ptr, argc_u as usize) else {
            return 1;
        };
        ids
    } else {
        vec![]
    };

    // All function calls go through V8. If the function was created by
    // napi_create_function, V8 will invoke generic_wasm_callback which
    // calls snapi_host_invoke_wasm_callback (Rust trampoline) → WASM dispatcher.
    // We set CB_ENV_PTR so the trampoline can access the WASM store.
    let env_ptr: *mut () = &mut env as *mut FunctionEnvMut<'_, RuntimeEnv> as *mut ();
    CB_ENV_PTR.with(|cell| cell.set(env_ptr));

    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_call_function(
            recv as u32,
            func as u32,
            argc_u,
            argv_ids.as_ptr(),
            &mut out,
        )
    };

    CB_ENV_PTR.with(|cell| cell.set(std::ptr::null_mut()));

    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// --- napi_create_function ---

fn guest_napi_create_function(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    name_ptr: i32,
    name_len: i32,
    cb: i32,
    data: i32,
    rp: i32,
) -> i32 {
    // Read function name
    let wl = name_len as u32;
    let name_bytes: Vec<u8> = if wl == 0xFFFFFFFFu32 {
        // NAPI_AUTO_LENGTH: read null-terminated string
        read_guest_c_string(&mut env, name_ptr).unwrap_or_default()
    } else if wl > 0 && name_ptr != 0 {
        let Some(bytes) = read_guest_bytes(&mut env, name_ptr, wl as usize) else {
            return 1;
        };
        bytes
    } else {
        vec![]
    };

    // Allocate a registration ID in the C++ callback registry
    let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };

    // Register the WASM callback and data pointer in the C++ registry
    unsafe { snapi_bridge_register_callback(reg_id, cb as u32, data as u64) };

    // Create a JS function in V8 with generic_wasm_callback as its native callback.
    // The reg_id is stored as the function's data pointer so generic_wasm_callback
    // can look up which WASM function to invoke.
    let c_name = CString::new(name_bytes).unwrap_or_default();
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_function(c_name.as_ptr(), wl, reg_id, &mut out) };
    if s != 0 {
        return s;
    }

    write_guest_u32(&mut env, rp as u32, out);
    0
}

// --- napi_get_cb_info ---

fn guest_napi_get_cb_info(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    _cbinfo: i32,
    argc_ptr: i32,
    argv_ptr: i32,
    this_ptr: i32,
    data_ptr: i32,
) -> i32 {
    // Read the caller's requested argc (size of their argv array)
    let wanted: u32 = if argc_ptr > 0 {
        let Some(bytes) = read_guest_bytes(&mut env, argc_ptr, 4) else {
            return 1;
        };
        u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
    } else {
        0
    };

    // Query the bridge for callback context
    let mut actual_argc: u32 = wanted;
    let mut argv_ids = vec![0u32; wanted as usize];
    let mut this_id: u32 = 0;
    let mut data_val: u64 = 0;

    let s = unsafe {
        snapi_bridge_get_cb_info(
            &mut actual_argc,
            if wanted > 0 {
                argv_ids.as_mut_ptr()
            } else {
                std::ptr::null_mut()
            },
            wanted,
            &mut this_id,
            &mut data_val,
        )
    };
    if s != 0 {
        return s;
    }

    // Write actual argc back
    if argc_ptr > 0 {
        write_guest_u32(&mut env, argc_ptr as u32, actual_argc);
    }

    // Write argv (array of handle IDs) - only write up to min(wanted, actual)
    if argv_ptr > 0 {
        let to_write = std::cmp::min(wanted, actual_argc);
        for i in 0..to_write {
            write_guest_u32(&mut env, (argv_ptr as u32) + i * 4, argv_ids[i as usize]);
        }
    }

    // Write this_arg
    if this_ptr > 0 {
        write_guest_u32(&mut env, this_ptr as u32, this_id);
    }

    // Write data pointer (as a 32-bit guest pointer)
    if data_ptr > 0 {
        write_guest_u32(&mut env, data_ptr as u32, data_val as u32);
    }

    0
}

// --- napi_get_new_target ---

fn guest_napi_get_new_target(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    _cbinfo: i32,
    rp: i32,
) -> i32 {
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_get_new_target(&mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// --- napi_define_class ---
// Guest layout of napi_property_descriptor (32-bit WASM, 32 bytes):
//   offset  0: const char* utf8name     (4 bytes, guest pointer)
//   offset  4: napi_value name          (4 bytes, handle ID)
//   offset  8: napi_callback method     (4 bytes, fn pointer)
//   offset 12: napi_callback getter     (4 bytes, fn pointer)
//   offset 16: napi_callback setter     (4 bytes, fn pointer)
//   offset 20: napi_value value         (4 bytes, handle ID)
//   offset 24: napi_property_attributes (4 bytes, enum)
//   offset 28: void* data               (4 bytes, guest pointer)
const PROP_DESC_SIZE: usize = 32;

fn guest_napi_define_class(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    name_ptr: i32,
    name_len: i32,
    constructor: i32,
    ctor_data: i32,
    prop_count: i32,
    props_ptr: i32,
    rp: i32,
) -> i32 {
    // Read class name
    let wl = name_len as u32;
    let name_bytes: Vec<u8> = if wl == 0xFFFFFFFFu32 {
        read_guest_c_string(&mut env, name_ptr).unwrap_or_default()
    } else if wl > 0 && name_ptr != 0 {
        let Some(bytes) = read_guest_bytes(&mut env, name_ptr, wl as usize) else {
            return 1;
        };
        bytes
    } else {
        vec![]
    };

    // Register the constructor callback
    let ctor_reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
    unsafe { snapi_bridge_register_callback(ctor_reg_id, constructor as u32, ctor_data as u64) };

    let pc = prop_count as u32;
    let c_name = CString::new(name_bytes).unwrap_or_default();

    if pc == 0 {
        // No properties — simple case
        let mut out: u32 = 0;
        let s = unsafe {
            snapi_bridge_define_class(
                c_name.as_ptr(),
                wl,
                ctor_reg_id,
                0,
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                &mut out,
            )
        };
        if s != 0 {
            return s;
        }
        write_guest_u32(&mut env, rp as u32, out);
        return 0;
    }

    // Read property descriptors from guest memory
    let total_bytes = pc as usize * PROP_DESC_SIZE;
    let Some(raw) = read_guest_bytes(&mut env, props_ptr, total_bytes) else {
        return 1;
    };

    let mut prop_names_c: Vec<CString> = Vec::with_capacity(pc as usize);
    let mut prop_names_ptrs: Vec<*const i8> = Vec::with_capacity(pc as usize);
    let mut prop_name_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_types: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_value_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_method_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_getter_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_setter_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_attributes: Vec<i32> = Vec::with_capacity(pc as usize);

    for i in 0..pc as usize {
        let base = i * PROP_DESC_SIZE;
        let utf8name_guest =
            u32::from_le_bytes([raw[base], raw[base + 1], raw[base + 2], raw[base + 3]]);
        let name_id =
            u32::from_le_bytes([raw[base + 4], raw[base + 5], raw[base + 6], raw[base + 7]]);
        let method_ptr =
            u32::from_le_bytes([raw[base + 8], raw[base + 9], raw[base + 10], raw[base + 11]]);
        let getter_ptr = u32::from_le_bytes([
            raw[base + 12],
            raw[base + 13],
            raw[base + 14],
            raw[base + 15],
        ]);
        let setter_ptr = u32::from_le_bytes([
            raw[base + 16],
            raw[base + 17],
            raw[base + 18],
            raw[base + 19],
        ]);
        let value_id = u32::from_le_bytes([
            raw[base + 20],
            raw[base + 21],
            raw[base + 22],
            raw[base + 23],
        ]);
        let attrs = i32::from_le_bytes([
            raw[base + 24],
            raw[base + 25],
            raw[base + 26],
            raw[base + 27],
        ]);
        let data_ptr = u32::from_le_bytes([
            raw[base + 28],
            raw[base + 29],
            raw[base + 30],
            raw[base + 31],
        ]);

        // Read property name
        let pname = if utf8name_guest != 0 {
            read_guest_c_string(&mut env, utf8name_guest as i32).unwrap_or_default()
        } else {
            vec![]
        };
        let c_pname = CString::new(pname).unwrap_or_default();
        prop_name_ids.push(name_id);

        // Determine property type and register callbacks as needed
        if method_ptr != 0 {
            // Method property
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
            unsafe { snapi_bridge_register_callback(reg_id, method_ptr, data_ptr as u64) };
            prop_types.push(1);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(reg_id);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(0);
        } else if getter_ptr != 0 && setter_ptr != 0 {
            // Getter + Setter
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
            unsafe {
                snapi_bridge_register_callback_pair(reg_id, getter_ptr, setter_ptr, data_ptr as u64)
            };
            prop_types.push(4);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(reg_id);
            prop_setter_reg_ids.push(0);
        } else if getter_ptr != 0 {
            // Getter only
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
            unsafe { snapi_bridge_register_callback(reg_id, getter_ptr, data_ptr as u64) };
            prop_types.push(2);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(reg_id);
            prop_setter_reg_ids.push(0);
        } else if setter_ptr != 0 {
            // Setter only
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
            unsafe { snapi_bridge_register_callback(reg_id, setter_ptr, data_ptr as u64) };
            prop_types.push(3);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(reg_id);
        } else {
            // Value property
            prop_types.push(0);
            prop_value_ids.push(value_id);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(0);
        }

        prop_attributes.push(attrs);
        prop_names_c.push(c_pname);
    }

    // Build pointer array (must live as long as the FFI call)
    for cn in &prop_names_c {
        prop_names_ptrs.push(cn.as_ptr());
    }

    let mut out: u32 = 0;
    let s = unsafe {
        snapi_bridge_define_class(
            c_name.as_ptr(),
            wl,
            ctor_reg_id,
            pc,
            prop_names_ptrs.as_ptr(),
            prop_name_ids.as_ptr(),
            prop_types.as_ptr(),
            prop_value_ids.as_ptr(),
            prop_method_reg_ids.as_ptr(),
            prop_getter_reg_ids.as_ptr(),
            prop_setter_reg_ids.as_ptr(),
            prop_attributes.as_ptr(),
            &mut out,
        )
    };
    if s != 0 {
        return s;
    }
    write_guest_u32(&mut env, rp as u32, out);
    0
}

fn guest_napi_define_properties(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    obj: i32,
    prop_count: i32,
    props_ptr: i32,
) -> i32 {
    let pc = prop_count as u32;
    if pc == 0 {
        return unsafe {
            snapi_bridge_define_properties(
                obj as u32,
                0,
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
            )
        };
    }

    let total_bytes = pc as usize * PROP_DESC_SIZE;
    let Some(raw) = read_guest_bytes(&mut env, props_ptr, total_bytes) else {
        return 1;
    };

    let mut prop_names_c: Vec<CString> = Vec::with_capacity(pc as usize);
    let mut prop_names_ptrs: Vec<*const i8> = Vec::with_capacity(pc as usize);
    let mut prop_name_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_types: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_value_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_method_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_getter_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_setter_reg_ids: Vec<u32> = Vec::with_capacity(pc as usize);
    let mut prop_attributes: Vec<i32> = Vec::with_capacity(pc as usize);

    for i in 0..pc as usize {
        let base = i * PROP_DESC_SIZE;
        let utf8name_guest =
            u32::from_le_bytes([raw[base], raw[base + 1], raw[base + 2], raw[base + 3]]);
        let name_id =
            u32::from_le_bytes([raw[base + 4], raw[base + 5], raw[base + 6], raw[base + 7]]);
        let method_ptr =
            u32::from_le_bytes([raw[base + 8], raw[base + 9], raw[base + 10], raw[base + 11]]);
        let getter_ptr = u32::from_le_bytes([
            raw[base + 12],
            raw[base + 13],
            raw[base + 14],
            raw[base + 15],
        ]);
        let setter_ptr = u32::from_le_bytes([
            raw[base + 16],
            raw[base + 17],
            raw[base + 18],
            raw[base + 19],
        ]);
        let value_id = u32::from_le_bytes([
            raw[base + 20],
            raw[base + 21],
            raw[base + 22],
            raw[base + 23],
        ]);
        let attrs = i32::from_le_bytes([
            raw[base + 24],
            raw[base + 25],
            raw[base + 26],
            raw[base + 27],
        ]);
        let data_ptr = u32::from_le_bytes([
            raw[base + 28],
            raw[base + 29],
            raw[base + 30],
            raw[base + 31],
        ]);

        let pname = if utf8name_guest != 0 {
            read_guest_c_string(&mut env, utf8name_guest as i32).unwrap_or_default()
        } else {
            vec![]
        };
        let c_pname = CString::new(pname).unwrap_or_default();
        prop_name_ids.push(name_id);

        if method_ptr != 0 {
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
            unsafe { snapi_bridge_register_callback(reg_id, method_ptr, data_ptr as u64) };
            prop_types.push(1);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(reg_id);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(0);
        } else if getter_ptr != 0 && setter_ptr != 0 {
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
            unsafe {
                snapi_bridge_register_callback_pair(reg_id, getter_ptr, setter_ptr, data_ptr as u64)
            };
            prop_types.push(4);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(reg_id);
            prop_setter_reg_ids.push(0);
        } else if getter_ptr != 0 {
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
            unsafe { snapi_bridge_register_callback(reg_id, getter_ptr, data_ptr as u64) };
            prop_types.push(2);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(reg_id);
            prop_setter_reg_ids.push(0);
        } else if setter_ptr != 0 {
            let reg_id = unsafe { snapi_bridge_alloc_cb_reg_id() };
            unsafe { snapi_bridge_register_callback(reg_id, setter_ptr, data_ptr as u64) };
            prop_types.push(3);
            prop_value_ids.push(0);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(reg_id);
        } else {
            prop_types.push(0);
            prop_value_ids.push(value_id);
            prop_method_reg_ids.push(0);
            prop_getter_reg_ids.push(0);
            prop_setter_reg_ids.push(0);
        }

        prop_attributes.push(attrs);
        prop_names_c.push(c_pname);
    }

    for cn in &prop_names_c {
        prop_names_ptrs.push(cn.as_ptr());
    }

    unsafe {
        snapi_bridge_define_properties(
            obj as u32,
            pc,
            prop_names_ptrs.as_ptr(),
            prop_name_ids.as_ptr(),
            prop_types.as_ptr(),
            prop_value_ids.as_ptr(),
            prop_method_reg_ids.as_ptr(),
            prop_getter_reg_ids.as_ptr(),
            prop_setter_reg_ids.as_ptr(),
            prop_attributes.as_ptr(),
        )
    }
}

// --- Script execution ---

fn guest_napi_run_script(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, sh: i32, rp: i32) -> i32 {
    // Set CB_ENV_PTR so scripts that trigger callbacks can trampoline back to WASM
    let env_ptr: *mut () = &mut env as *mut FunctionEnvMut<'_, RuntimeEnv> as *mut ();
    CB_ENV_PTR.with(|cell| cell.set(env_ptr));

    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_run_script(sh as u32, &mut out) };

    CB_ENV_PTR.with(|cell| cell.set(std::ptr::null_mut()));

    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// --- UTF-16 strings ---

fn guest_napi_create_string_utf16(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    str_ptr: i32,
    length: i32,
    rp: i32,
) -> i32 {
    let wl = length as u32;
    // UTF-16: each char is 2 bytes
    let char_count: usize = if wl == 0xFFFFFFFFu32 {
        // Auto-length: scan for null terminator (u16 == 0)
        let mut scan_len: usize = 0;
        loop {
            let Some(bytes) = read_guest_bytes(&mut env, str_ptr + (scan_len as i32 * 2), 2) else {
                break;
            };
            let ch = u16::from_le_bytes([bytes[0], bytes[1]]);
            if ch == 0 {
                break;
            }
            scan_len += 1;
            if scan_len > MAX_GUEST_CSTRING_SCAN {
                break;
            }
        }
        scan_len
    } else {
        wl as usize
    };
    let byte_len = char_count * 2;
    let Some(raw_bytes) = read_guest_bytes(&mut env, str_ptr, byte_len) else {
        return 1;
    };
    // Convert bytes to u16 array
    let u16_data: Vec<u16> = raw_bytes
        .chunks_exact(2)
        .map(|c| u16::from_le_bytes([c[0], c[1]]))
        .collect();
    let mut out: u32 = 0;
    // Always pass the actual char count to the 64-bit bridge (not the WASM32 NAPI_AUTO_LENGTH sentinel)
    let s =
        unsafe { snapi_bridge_create_string_utf16(u16_data.as_ptr(), char_count as u32, &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_value_string_utf16(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    bp: i32,
    bs: i32,
    rp: i32,
) -> i32 {
    let hbs = if bs <= 0 { 0usize } else { bs as usize };
    let mut hb = vec![0u16; hbs];
    let mut rl: usize = 0;
    let s = unsafe {
        snapi_bridge_get_value_string_utf16(
            vh as u32,
            if hbs > 0 {
                hb.as_mut_ptr()
            } else {
                std::ptr::null_mut()
            },
            hbs,
            &mut rl,
        )
    };
    if s != 0 {
        return s;
    }
    if bp > 0 && hbs > 0 {
        let n = hbs.min(rl + 1);
        // Write u16 values as LE bytes to guest memory
        let bytes: Vec<u8> = hb[..n].iter().flat_map(|&v| v.to_le_bytes()).collect();
        write_guest_bytes(&mut env, bp as u32, &bytes);
    }
    if rp > 0 {
        write_guest_u32(&mut env, rp as u32, rl as u32);
    }
    0
}

// --- BigInt words ---

fn guest_napi_create_bigint_words(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    sign_bit: i32,
    word_count: i32,
    words_ptr: i32,
    rp: i32,
) -> i32 {
    let wc = word_count as u32;
    // Read u64 words from guest memory (each is 8 bytes)
    let Some(words_bytes) = read_guest_bytes(&mut env, words_ptr, wc as usize * 8) else {
        return 1;
    };
    let words: Vec<u64> = words_bytes
        .chunks_exact(8)
        .map(|c| u64::from_le_bytes(c.try_into().unwrap()))
        .collect();
    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_create_bigint_words(sign_bit, wc, words.as_ptr(), &mut out) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

fn guest_napi_get_value_bigint_words(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    sign_ptr: i32,
    wc_ptr: i32,
    words_ptr: i32,
) -> i32 {
    // First, read the word_count from guest to know how many to allocate
    let Some(wc_bytes) = read_guest_bytes(&mut env, wc_ptr, 4) else {
        return 1;
    };
    let mut word_count = u32::from_le_bytes(wc_bytes.try_into().unwrap()) as usize;

    if words_ptr <= 0 {
        // Query mode: just get the word count
        let mut sign: i32 = 0;
        let s = unsafe {
            snapi_bridge_get_value_bigint_words(
                vh as u32,
                &mut sign,
                &mut word_count,
                std::ptr::null_mut(),
            )
        };
        if s == 0 {
            write_guest_i32(&mut env, sign_ptr as u32, sign);
            write_guest_u32(&mut env, wc_ptr as u32, word_count as u32);
        }
        return s;
    }

    let mut sign: i32 = 0;
    let mut words = vec![0u64; word_count];
    let s = unsafe {
        snapi_bridge_get_value_bigint_words(
            vh as u32,
            &mut sign,
            &mut word_count,
            words.as_mut_ptr(),
        )
    };
    if s == 0 {
        write_guest_i32(&mut env, sign_ptr as u32, sign);
        write_guest_u32(&mut env, wc_ptr as u32, word_count as u32);
        // Write u64 words as LE bytes to guest
        let bytes: Vec<u8> = words[..word_count]
            .iter()
            .flat_map(|&v| v.to_le_bytes())
            .collect();
        write_guest_bytes(&mut env, words_ptr as u32, &bytes);
    }
    s
}

// --- Instance data ---

fn guest_napi_set_instance_data(
    _env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    data: i32,
    _finalize_cb: i32,
    _finalize_hint: i32,
) -> i32 {
    unsafe { snapi_bridge_set_instance_data(data as u64) }
}

fn guest_napi_get_instance_data(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_get_instance_data(&mut data) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, data as u32);
    }
    s
}

fn guest_napi_adjust_external_memory(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    change: i64,
    rp: i32,
) -> i32 {
    let mut adjusted: i64 = 0;
    let s = unsafe { snapi_bridge_adjust_external_memory(change, &mut adjusted) };
    if s == 0 {
        write_guest_i64(&mut env, rp as u32, adjusted);
    }
    s
}

// --- Node Buffers ---

fn guest_napi_create_buffer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    length: i32,
    data_ptr: i32,
    rp: i32,
) -> i32 {
    // Buffers must be backed by guest linear memory (same pattern as create_arraybuffer)
    let malloc_fn = env.data().malloc_fn.clone();
    let memory = env.data().memory.clone();

    if let (Some(malloc_fn), Some(memory)) = (malloc_fn, memory) {
        // Allocate memory in the guest's linear memory
        let guest_ptr: i32 = {
            let (_, mut store_ref) = env.data_and_store_mut();
            match malloc_fn.call(&mut store_ref, length) {
                Ok(ptr) if ptr > 0 => ptr,
                _ => return 1,
            }
        };

        // Get host pointer corresponding to the guest allocation
        let host_addr: u64 = {
            let (_, store_ref) = env.data_and_store_mut();
            let view = memory.view(&store_ref);
            let host_base = view.data_ptr() as u64;
            host_base + guest_ptr as u64
        };

        // Zero-initialize the guest memory region
        if length > 0 {
            let zeros = vec![0u8; length as usize];
            write_guest_bytes(&mut env, guest_ptr as u32, &zeros);
        }

        let mut buf_id: u32 = 0;
        let s =
            unsafe { snapi_bridge_create_external_buffer(host_addr, length as u32, &mut buf_id) };
        if s != 0 {
            return s;
        }

        // Store mapping from buffer handle ID → guest data pointer
        env.data_mut()
            .guest_data_ptrs
            .insert(buf_id, guest_ptr as u32);

        write_guest_u32(&mut env, rp as u32, buf_id);
        if data_ptr > 0 {
            write_guest_u32(&mut env, data_ptr as u32, guest_ptr as u32);
        }
        0
    } else {
        // Fallback for non-WASIX: use bridge directly
        let mut host_data: u64 = 0;
        let mut out: u32 = 0;
        let s = unsafe { snapi_bridge_create_buffer(length as u32, &mut host_data, &mut out) };
        if s != 0 {
            return s;
        }
        write_guest_u32(&mut env, rp as u32, out);
        0
    }
}

fn guest_napi_create_buffer_copy(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    length: i32,
    data_ptr: i32,
    result_data_ptr: i32,
    rp: i32,
) -> i32 {
    // Read source data from guest memory first
    let Some(src_data) = read_guest_bytes(&mut env, data_ptr, length as usize) else {
        return 1;
    };

    let malloc_fn = env.data().malloc_fn.clone();
    let memory = env.data().memory.clone();

    if let (Some(malloc_fn), Some(memory)) = (malloc_fn, memory) {
        // Allocate memory in the guest's linear memory
        let guest_ptr: i32 = {
            let (_, mut store_ref) = env.data_and_store_mut();
            match malloc_fn.call(&mut store_ref, length) {
                Ok(ptr) if ptr > 0 => ptr,
                _ => return 1,
            }
        };

        // Copy source data to guest memory
        write_guest_bytes(&mut env, guest_ptr as u32, &src_data);

        // Get host pointer corresponding to the guest allocation
        let host_addr: u64 = {
            let (_, store_ref) = env.data_and_store_mut();
            let view = memory.view(&store_ref);
            let host_base = view.data_ptr() as u64;
            host_base + guest_ptr as u64
        };

        let mut buf_id: u32 = 0;
        let s =
            unsafe { snapi_bridge_create_external_buffer(host_addr, length as u32, &mut buf_id) };
        if s != 0 {
            return s;
        }

        // Store mapping from buffer handle ID → guest data pointer
        env.data_mut()
            .guest_data_ptrs
            .insert(buf_id, guest_ptr as u32);

        write_guest_u32(&mut env, rp as u32, buf_id);
        if result_data_ptr > 0 {
            write_guest_u32(&mut env, result_data_ptr as u32, guest_ptr as u32);
        }
        0
    } else {
        // Fallback for non-WASIX
        let mut result_host_data: u64 = 0;
        let mut out: u32 = 0;
        let s = unsafe {
            snapi_bridge_create_buffer_copy(
                length as u32,
                src_data.as_ptr(),
                &mut result_host_data,
                &mut out,
            )
        };
        if s == 0 {
            write_guest_u32(&mut env, rp as u32, out);
        }
        s
    }
}

guest_is_check!(guest_napi_is_buffer, snapi_bridge_is_buffer);

fn guest_napi_get_buffer_info(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    vh: i32,
    data_ptr: i32,
    len_ptr: i32,
) -> i32 {
    let mut host_data: u64 = 0;
    let mut bl: u32 = 0;
    let s = unsafe { snapi_bridge_get_buffer_info(vh as u32, &mut host_data, &mut bl) };
    if s != 0 {
        return s;
    }
    if len_ptr > 0 {
        write_guest_u32(&mut env, len_ptr as u32, bl);
    }
    if data_ptr > 0 {
        if let Some(guest_data_ptr) =
            resolve_or_copy_host_data_to_guest(&mut env, vh as u32, host_data, bl as usize)
        {
            write_guest_u32(&mut env, data_ptr as u32, guest_data_ptr);
        }
    }
    0
}

// --- Node version ---

fn guest_napi_get_node_version(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    let mut major: u32 = 0;
    let mut minor: u32 = 0;
    let mut patch: u32 = 0;
    let s = unsafe { snapi_bridge_get_node_version(&mut major, &mut minor, &mut patch) };
    if s != 0 {
        return s;
    }
    // Write a napi_node_version struct to guest memory:
    // { uint32_t major, uint32_t minor, uint32_t patch, const char* release }
    // For WASM: we write the struct (16 bytes) into a static-ish location
    // But the N-API spec says we return a *pointer* to the version struct.
    // Actually, the API signature is: napi_get_node_version(env, const napi_node_version** version)
    // So we need to allocate memory in guest for the struct and write the pointer.
    // For simplicity, use malloc if available, otherwise just write the pointer to a fixed value.
    let malloc_fn = env.data().malloc_fn.clone();
    if let Some(malloc_fn) = malloc_fn {
        // Allocate 16 bytes for the struct (major, minor, patch, release_ptr)
        // Then allocate a small buffer for the release string
        let release_str = b"napi-external\0";
        let struct_size = 16i32; // 4 * u32 = 16 (release is a pointer)
        let total = struct_size + release_str.len() as i32;
        let guest_ptr: i32 = {
            let (_, mut store_ref) = env.data_and_store_mut();
            match malloc_fn.call(&mut store_ref, total) {
                Ok(ptr) if ptr > 0 => ptr,
                _ => return 1,
            }
        };
        // Write release string
        let release_offset = guest_ptr + struct_size;
        write_guest_bytes(&mut env, release_offset as u32, release_str);
        // Write struct fields
        write_guest_u32(&mut env, guest_ptr as u32, major);
        write_guest_u32(&mut env, (guest_ptr + 4) as u32, minor);
        write_guest_u32(&mut env, (guest_ptr + 8) as u32, patch);
        write_guest_u32(&mut env, (guest_ptr + 12) as u32, release_offset as u32);
        // Write pointer to struct
        write_guest_u32(&mut env, rp as u32, guest_ptr as u32);
    } else {
        // Fallback: just write major version as a simple value
        write_guest_u32(&mut env, rp as u32, major);
    }
    0
}

// --- Object wrapping ---

fn guest_napi_wrap(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    obj: i32,
    native_data: i32,
    _finalize_cb: i32,
    _finalize_hint: i32,
    ref_ptr: i32,
) -> i32 {
    let mut ref_out: u32 = 0;
    let s = if ref_ptr > 0 {
        unsafe { snapi_bridge_wrap(obj as u32, native_data as u64, &mut ref_out) }
    } else {
        unsafe { snapi_bridge_wrap(obj as u32, native_data as u64, std::ptr::null_mut()) }
    };
    if s == 0 && ref_ptr > 0 {
        write_guest_u32(&mut env, ref_ptr as u32, ref_out);
    }
    s
}

fn guest_napi_unwrap(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, obj: i32, rp: i32) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_unwrap(obj as u32, &mut data) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, data as u32);
    }
    s
}

fn guest_napi_remove_wrap(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, obj: i32, rp: i32) -> i32 {
    let mut data: u64 = 0;
    let s = unsafe { snapi_bridge_remove_wrap(obj as u32, &mut data) };
    if s == 0 {
        write_guest_u32(&mut env, rp as u32, data as u32);
    }
    s
}

fn guest_napi_add_finalizer(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    obj: i32,
    data: i32,
    _finalize_cb: i32,
    _finalize_hint: i32,
    ref_ptr: i32,
) -> i32 {
    let mut ref_out: u32 = 0;
    let s = if ref_ptr > 0 {
        unsafe { snapi_bridge_add_finalizer(obj as u32, data as u64, &mut ref_out) }
    } else {
        unsafe { snapi_bridge_add_finalizer(obj as u32, data as u64, std::ptr::null_mut()) }
    };
    if s == 0 && ref_ptr > 0 {
        write_guest_u32(&mut env, ref_ptr as u32, ref_out);
    }
    s
}

// --- Misc stubs ---

fn guest_napi_get_last_error_info(_env: FunctionEnvMut<RuntimeEnv>, _e: i32, _rp: i32) -> i32 {
    0
}

fn guest_napi_get_version(mut env: FunctionEnvMut<RuntimeEnv>, _e: i32, rp: i32) -> i32 {
    write_guest_u32(&mut env, rp as u32, 8);
    0
}

fn guest_napi_fatal_error(
    mut env: FunctionEnvMut<RuntimeEnv>,
    loc_ptr: i32,
    loc_len: i32,
    msg_ptr: i32,
    msg_len: i32,
) -> i32 {
    // Read location and message from guest memory
    let loc = if loc_ptr > 0 {
        let len = if loc_len as u32 == 0xFFFFFFFFu32 {
            read_guest_c_string(&mut env, loc_ptr)
                .map(|v| v.len())
                .unwrap_or(0)
        } else {
            loc_len as usize
        };
        read_guest_bytes(&mut env, loc_ptr, len).map(|b| String::from_utf8_lossy(&b).to_string())
    } else {
        None
    };
    let msg = if msg_ptr > 0 {
        let len = if msg_len as u32 == 0xFFFFFFFFu32 {
            read_guest_c_string(&mut env, msg_ptr)
                .map(|v| v.len())
                .unwrap_or(0)
        } else {
            msg_len as usize
        };
        read_guest_bytes(&mut env, msg_ptr, len).map(|b| String::from_utf8_lossy(&b).to_string())
    } else {
        None
    };
    eprintln!(
        "FATAL ERROR: location={}, message={}",
        loc.as_deref().unwrap_or("(null)"),
        msg.as_deref().unwrap_or("(null)")
    );
    std::process::abort();
}

// --- Constructor ---

fn guest_napi_new_instance(
    mut env: FunctionEnvMut<RuntimeEnv>,
    _e: i32,
    ctor: i32,
    argc: i32,
    argv_ptr: i32,
    rp: i32,
) -> i32 {
    let argc_u = argc as u32;
    let argv_ids = if argc_u > 0 {
        let Some(ids) = read_guest_u32_array(&mut env, argv_ptr, argc_u as usize) else {
            return 1;
        };
        ids
    } else {
        vec![]
    };

    // Set CB_ENV_PTR so constructor callbacks can trampoline back to WASM
    let env_ptr: *mut () = &mut env as *mut FunctionEnvMut<'_, RuntimeEnv> as *mut ();
    CB_ENV_PTR.with(|cell| cell.set(env_ptr));

    let mut out: u32 = 0;
    let s = unsafe { snapi_bridge_new_instance(ctor as u32, argc_u, argv_ids.as_ptr(), &mut out) };

    CB_ENV_PTR.with(|cell| cell.set(std::ptr::null_mut()));

    if s == 0 {
        write_guest_u32(&mut env, rp as u32, out);
    }
    s
}

// ============================================================
// Register all WASM imports for the "napi" module
// ============================================================

pub fn register_napi_imports(store: &mut Store, fe: &FunctionEnv<RuntimeEnv>, io: &mut Imports) {
    macro_rules! reg {
        ($name:expr, $func:expr) => {
            io.define(
                "napi",
                $name,
                Function::new_typed_with_env(store, fe, $func),
            );
        };
    }

    // Init
    reg!("napi_wasm_init_env", guest_napi_wasm_init_env);
    reg!(
        "unofficial_napi_create_env",
        guest_unofficial_napi_create_env
    );
    reg!(
        "unofficial_napi_release_env",
        guest_unofficial_napi_release_env
    );
    reg!(
        "unofficial_napi_process_microtasks",
        guest_unofficial_napi_process_microtasks
    );
    reg!(
        "unofficial_napi_request_gc_for_testing",
        guest_unofficial_napi_request_gc_for_testing
    );
    reg!(
        "unofficial_napi_get_promise_details",
        guest_unofficial_napi_get_promise_details
    );
    reg!(
        "unofficial_napi_get_proxy_details",
        guest_unofficial_napi_get_proxy_details
    );
    reg!(
        "unofficial_napi_preview_entries",
        guest_unofficial_napi_preview_entries
    );
    reg!(
        "unofficial_napi_get_call_sites",
        guest_unofficial_napi_get_call_sites
    );
    reg!(
        "unofficial_napi_get_caller_location",
        guest_unofficial_napi_get_caller_location
    );
    reg!(
        "unofficial_napi_arraybuffer_view_has_buffer",
        guest_unofficial_napi_arraybuffer_view_has_buffer
    );
    reg!(
        "unofficial_napi_get_constructor_name",
        guest_unofficial_napi_get_constructor_name
    );
    reg!(
        "unofficial_napi_create_private_symbol",
        guest_unofficial_napi_create_private_symbol
    );
    reg!(
        "unofficial_napi_get_continuation_preserved_embedder_data",
        guest_unofficial_napi_get_continuation_preserved_embedder_data
    );
    reg!(
        "unofficial_napi_set_continuation_preserved_embedder_data",
        guest_unofficial_napi_set_continuation_preserved_embedder_data
    );
    reg!(
        "unofficial_napi_notify_datetime_configuration_change",
        guest_unofficial_napi_notify_datetime_configuration_change
    );
    reg!(
        "unofficial_napi_set_enqueue_foreground_task_callback",
        guest_unofficial_napi_set_enqueue_foreground_task_callback
    );
    reg!(
        "unofficial_napi_set_fatal_error_callbacks",
        guest_unofficial_napi_set_fatal_error_callbacks
    );
    reg!(
        "unofficial_napi_terminate_execution",
        guest_unofficial_napi_terminate_execution
    );
    reg!(
        "unofficial_napi_structured_clone",
        guest_unofficial_napi_structured_clone
    );
    reg!(
        "unofficial_napi_serialize_value",
        guest_unofficial_napi_serialize_value
    );
    reg!(
        "unofficial_napi_deserialize_value",
        guest_unofficial_napi_deserialize_value
    );
    reg!(
        "unofficial_napi_release_serialized_value",
        guest_unofficial_napi_release_serialized_value
    );
    reg!(
        "unofficial_napi_enqueue_microtask",
        guest_unofficial_napi_enqueue_microtask
    );
    reg!(
        "unofficial_napi_set_promise_reject_callback",
        guest_unofficial_napi_set_promise_reject_callback
    );
    reg!(
        "unofficial_napi_get_own_non_index_properties",
        guest_unofficial_napi_get_own_non_index_properties
    );
    reg!(
        "unofficial_napi_get_process_memory_info",
        guest_unofficial_napi_get_process_memory_info
    );
    reg!(
        "unofficial_napi_create_serdes_binding",
        guest_unofficial_napi_create_serdes_binding
    );
    reg!(
        "unofficial_napi_contextify_contains_module_syntax",
        guest_unofficial_napi_contextify_contains_module_syntax
    );
    reg!(
        "unofficial_napi_contextify_make_context",
        guest_unofficial_napi_contextify_make_context
    );
    reg!(
        "unofficial_napi_contextify_run_script",
        guest_unofficial_napi_contextify_run_script
    );
    reg!(
        "unofficial_napi_contextify_dispose_context",
        guest_unofficial_napi_contextify_dispose_context
    );
    reg!(
        "unofficial_napi_contextify_compile_function",
        guest_unofficial_napi_contextify_compile_function
    );
    reg!(
        "unofficial_napi_contextify_compile_function_for_cjs_loader",
        guest_unofficial_napi_contextify_compile_function_for_cjs_loader
    );
    reg!(
        "unofficial_napi_contextify_create_cached_data",
        guest_unofficial_napi_contextify_create_cached_data
    );
    reg!(
        "unofficial_napi_module_wrap_create_source_text",
        guest_unofficial_napi_module_wrap_create_source_text
    );
    reg!(
        "unofficial_napi_module_wrap_create_synthetic",
        guest_unofficial_napi_module_wrap_create_synthetic
    );
    reg!(
        "unofficial_napi_module_wrap_destroy",
        guest_unofficial_napi_module_wrap_destroy
    );
    reg!(
        "unofficial_napi_module_wrap_get_module_requests",
        guest_unofficial_napi_module_wrap_get_module_requests
    );
    reg!(
        "unofficial_napi_module_wrap_link",
        guest_unofficial_napi_module_wrap_link
    );
    reg!(
        "unofficial_napi_module_wrap_instantiate",
        guest_unofficial_napi_module_wrap_instantiate
    );
    reg!(
        "unofficial_napi_module_wrap_evaluate",
        guest_unofficial_napi_module_wrap_evaluate
    );
    reg!(
        "unofficial_napi_module_wrap_evaluate_sync",
        guest_unofficial_napi_module_wrap_evaluate_sync
    );
    reg!(
        "unofficial_napi_module_wrap_get_namespace",
        guest_unofficial_napi_module_wrap_get_namespace
    );
    reg!(
        "unofficial_napi_module_wrap_get_status",
        guest_unofficial_napi_module_wrap_get_status
    );
    reg!(
        "unofficial_napi_module_wrap_get_error",
        guest_unofficial_napi_module_wrap_get_error
    );
    reg!(
        "unofficial_napi_module_wrap_has_top_level_await",
        guest_unofficial_napi_module_wrap_has_top_level_await
    );
    reg!(
        "unofficial_napi_module_wrap_has_async_graph",
        guest_unofficial_napi_module_wrap_has_async_graph
    );
    reg!(
        "unofficial_napi_module_wrap_set_export",
        guest_unofficial_napi_module_wrap_set_export
    );
    reg!(
        "unofficial_napi_module_wrap_set_module_source_object",
        guest_unofficial_napi_module_wrap_set_module_source_object
    );
    reg!(
        "unofficial_napi_module_wrap_get_module_source_object",
        guest_unofficial_napi_module_wrap_get_module_source_object
    );
    reg!(
        "unofficial_napi_module_wrap_create_cached_data",
        guest_unofficial_napi_module_wrap_create_cached_data
    );
    reg!(
        "unofficial_napi_module_wrap_set_import_module_dynamically_callback",
        guest_unofficial_napi_module_wrap_set_import_module_dynamically_callback
    );
    reg!(
        "unofficial_napi_module_wrap_set_initialize_import_meta_object_callback",
        guest_unofficial_napi_module_wrap_set_initialize_import_meta_object_callback
    );
    reg!(
        "unofficial_napi_module_wrap_import_module_dynamically",
        guest_unofficial_napi_module_wrap_import_module_dynamically
    );
    reg!(
        "unofficial_napi_module_wrap_create_required_module_facade",
        guest_unofficial_napi_module_wrap_create_required_module_facade
    );
    // Singleton getters
    reg!("napi_get_undefined", guest_napi_get_undefined);
    reg!("napi_get_null", guest_napi_get_null);
    reg!("napi_get_boolean", guest_napi_get_boolean);
    reg!("napi_get_global", guest_napi_get_global);
    // Value creation
    reg!("napi_create_string_utf8", guest_napi_create_string_utf8);
    reg!("napi_create_string_latin1", guest_napi_create_string_latin1);
    reg!("napi_create_int32", guest_napi_create_int32);
    reg!("napi_create_uint32", guest_napi_create_uint32);
    reg!("napi_create_double", guest_napi_create_double);
    reg!("napi_create_int64", guest_napi_create_int64);
    reg!("napi_create_object", guest_napi_create_object);
    reg!("napi_create_array", guest_napi_create_array);
    reg!(
        "napi_create_array_with_length",
        guest_napi_create_array_with_length
    );
    reg!("napi_create_symbol", guest_napi_create_symbol);
    reg!("napi_create_error", guest_napi_create_error);
    reg!("napi_create_type_error", guest_napi_create_type_error);
    reg!("napi_create_range_error", guest_napi_create_range_error);
    reg!("napi_create_bigint_int64", guest_napi_create_bigint_int64);
    reg!("napi_create_bigint_uint64", guest_napi_create_bigint_uint64);
    reg!("napi_create_date", guest_napi_create_date);
    reg!("napi_create_external", guest_napi_create_external);
    reg!("napi_create_arraybuffer", guest_napi_create_arraybuffer);
    reg!(
        "napi_create_external_arraybuffer",
        guest_napi_create_external_arraybuffer
    );
    reg!(
        "napi_create_external_buffer",
        guest_napi_create_external_buffer
    );
    reg!("napi_create_typedarray", guest_napi_create_typedarray);
    reg!("napi_create_dataview", guest_napi_create_dataview);
    reg!("napi_create_promise", guest_napi_create_promise);
    // Value reading
    reg!(
        "napi_get_value_string_utf8",
        guest_napi_get_value_string_utf8
    );
    reg!(
        "napi_get_value_string_latin1",
        guest_napi_get_value_string_latin1
    );
    reg!("napi_get_value_int32", guest_napi_get_value_int32);
    reg!("napi_get_value_uint32", guest_napi_get_value_uint32);
    reg!("napi_get_value_double", guest_napi_get_value_double);
    reg!("napi_get_value_int64", guest_napi_get_value_int64);
    reg!("napi_get_value_bool", guest_napi_get_value_bool);
    reg!(
        "napi_get_value_bigint_int64",
        guest_napi_get_value_bigint_int64
    );
    reg!(
        "napi_get_value_bigint_uint64",
        guest_napi_get_value_bigint_uint64
    );
    reg!("napi_get_date_value", guest_napi_get_date_value);
    reg!("napi_get_value_external", guest_napi_get_value_external);
    // Type checking
    reg!("napi_typeof", guest_napi_typeof);
    reg!("napi_is_array", guest_napi_is_array);
    reg!("napi_is_error", guest_napi_is_error);
    reg!("napi_is_arraybuffer", guest_napi_is_arraybuffer);
    reg!("napi_is_typedarray", guest_napi_is_typedarray);
    reg!("napi_is_dataview", guest_napi_is_dataview);
    reg!("napi_is_date", guest_napi_is_date);
    reg!("napi_is_promise", guest_napi_is_promise);
    reg!("napi_instanceof", guest_napi_instanceof);
    // Coercion
    reg!("napi_coerce_to_bool", guest_napi_coerce_to_bool);
    reg!("napi_coerce_to_number", guest_napi_coerce_to_number);
    reg!("napi_coerce_to_string", guest_napi_coerce_to_string);
    reg!("napi_coerce_to_object", guest_napi_coerce_to_object);
    // Object operations
    reg!("napi_set_property", guest_napi_set_property);
    reg!("napi_get_property", guest_napi_get_property);
    reg!("napi_has_property", guest_napi_has_property);
    reg!("napi_has_own_property", guest_napi_has_own_property);
    reg!("napi_delete_property", guest_napi_delete_property);
    reg!("napi_set_named_property", guest_napi_set_named_property);
    reg!("napi_get_named_property", guest_napi_get_named_property);
    reg!("napi_has_named_property", guest_napi_has_named_property);
    reg!("napi_set_element", guest_napi_set_element);
    reg!("napi_get_element", guest_napi_get_element);
    reg!("napi_has_element", guest_napi_has_element);
    reg!("napi_delete_element", guest_napi_delete_element);
    reg!("napi_get_array_length", guest_napi_get_array_length);
    reg!("napi_get_property_names", guest_napi_get_property_names);
    reg!(
        "napi_get_all_property_names",
        guest_napi_get_all_property_names
    );
    reg!("napi_get_prototype", guest_napi_get_prototype);
    reg!("napi_object_freeze", guest_napi_object_freeze);
    reg!("napi_object_seal", guest_napi_object_seal);
    // Comparison
    reg!("napi_strict_equals", guest_napi_strict_equals);
    // Error handling
    reg!("napi_throw", guest_napi_throw);
    reg!("napi_throw_error", guest_napi_throw_error);
    reg!("napi_throw_type_error", guest_napi_throw_type_error);
    reg!("napi_throw_range_error", guest_napi_throw_range_error);
    reg!("napi_is_exception_pending", guest_napi_is_exception_pending);
    reg!(
        "napi_get_and_clear_last_exception",
        guest_napi_get_and_clear_last_exception
    );
    // Promise
    reg!("napi_resolve_deferred", guest_napi_resolve_deferred);
    reg!("napi_reject_deferred", guest_napi_reject_deferred);
    // ArrayBuffer
    reg!("napi_get_arraybuffer_info", guest_napi_get_arraybuffer_info);
    reg!("napi_detach_arraybuffer", guest_napi_detach_arraybuffer);
    reg!(
        "napi_is_detached_arraybuffer",
        guest_napi_is_detached_arraybuffer
    );
    reg!(
        "node_api_is_sharedarraybuffer",
        guest_node_api_is_sharedarraybuffer
    );
    reg!(
        "node_api_create_sharedarraybuffer",
        guest_node_api_create_sharedarraybuffer
    );
    reg!("node_api_set_prototype", guest_node_api_set_prototype);
    // TypedArray
    reg!("napi_get_typedarray_info", guest_napi_get_typedarray_info);
    // DataView
    reg!("napi_get_dataview_info", guest_napi_get_dataview_info);
    // References
    reg!("napi_create_reference", guest_napi_create_reference);
    reg!("napi_delete_reference", guest_napi_delete_reference);
    reg!("napi_reference_ref", guest_napi_reference_ref);
    reg!("napi_reference_unref", guest_napi_reference_unref);
    reg!("napi_get_reference_value", guest_napi_get_reference_value);
    // Handle scopes
    reg!("napi_open_handle_scope", guest_napi_open_handle_scope);
    reg!("napi_close_handle_scope", guest_napi_close_handle_scope);
    reg!(
        "napi_open_escapable_handle_scope",
        guest_napi_open_escapable_handle_scope
    );
    reg!(
        "napi_close_escapable_handle_scope",
        guest_napi_close_escapable_handle_scope
    );
    reg!("napi_escape_handle", guest_napi_escape_handle);
    // Type tagging
    reg!("napi_type_tag_object", guest_napi_type_tag_object);
    reg!(
        "napi_check_object_type_tag",
        guest_napi_check_object_type_tag
    );
    // Function calling
    reg!("napi_call_function", guest_napi_call_function);
    reg!("napi_create_function", guest_napi_create_function);
    reg!("napi_get_cb_info", guest_napi_get_cb_info);
    reg!("napi_get_new_target", guest_napi_get_new_target);
    // Script execution
    reg!("napi_run_script", guest_napi_run_script);
    // UTF-16 strings
    reg!("napi_create_string_utf16", guest_napi_create_string_utf16);
    reg!(
        "napi_get_value_string_utf16",
        guest_napi_get_value_string_utf16
    );
    // BigInt words
    reg!("napi_create_bigint_words", guest_napi_create_bigint_words);
    reg!(
        "napi_get_value_bigint_words",
        guest_napi_get_value_bigint_words
    );
    // Instance data
    reg!("napi_set_instance_data", guest_napi_set_instance_data);
    reg!("napi_get_instance_data", guest_napi_get_instance_data);
    reg!(
        "napi_adjust_external_memory",
        guest_napi_adjust_external_memory
    );
    // Node Buffers
    reg!("napi_create_buffer", guest_napi_create_buffer);
    reg!("napi_create_buffer_copy", guest_napi_create_buffer_copy);
    reg!("napi_is_buffer", guest_napi_is_buffer);
    reg!("napi_get_buffer_info", guest_napi_get_buffer_info);
    // Node version
    reg!("napi_get_node_version", guest_napi_get_node_version);
    // Object wrapping
    reg!("napi_wrap", guest_napi_wrap);
    reg!("napi_unwrap", guest_napi_unwrap);
    reg!("napi_remove_wrap", guest_napi_remove_wrap);
    reg!("napi_add_finalizer", guest_napi_add_finalizer);
    // Constructor / Class
    reg!("napi_new_instance", guest_napi_new_instance);
    reg!("napi_define_properties", guest_napi_define_properties);
    reg!("napi_define_class", guest_napi_define_class);
    // Fatal error
    reg!("napi_fatal_error", guest_napi_fatal_error);
    // Misc
    reg!("napi_get_last_error_info", guest_napi_get_last_error_info);
    reg!("napi_get_version", guest_napi_get_version);
    reg!("napi_add_env_cleanup_hook", guest_napi_add_env_cleanup_hook);
    reg!(
        "napi_remove_env_cleanup_hook",
        guest_napi_remove_env_cleanup_hook
    );
}

fn guest_env_uv_cpu_info(_cpu_infos_out: i32, _count_out: i32) -> i32 {
    -1
}

fn guest_env_uv_interface_addresses(_addresses_out: i32, _count_out: i32) -> i32 {
    -1
}

fn guest_env_uv_free_interface_addresses(_addresses: i32, _count: i32) {}

fn guest_env_uv_resident_set_memory(_rss_out: i32) -> i32 {
    -1
}

fn guest_env_uv_get_free_memory() -> i64 {
    0
}

fn guest_env_uv_get_total_memory() -> i64 {
    0
}

fn guest_env_uv_get_available_memory() -> i64 {
    0
}

fn guest_env_uv_get_constrained_memory() -> i64 {
    0
}

pub fn register_env_imports(store: &mut Store, io: &mut Imports) {
    macro_rules! reg_env {
        ($name:expr, $func:expr) => {
            io.define("env", $name, Function::new_typed(store, $func));
        };
    }

    reg_env!("uv_cpu_info", guest_env_uv_cpu_info);
    reg_env!("uv_interface_addresses", guest_env_uv_interface_addresses);
    reg_env!(
        "uv_free_interface_addresses",
        guest_env_uv_free_interface_addresses
    );
    reg_env!("uv_resident_set_memory", guest_env_uv_resident_set_memory);
    reg_env!("uv_get_free_memory", guest_env_uv_get_free_memory);
    reg_env!("uv_get_total_memory", guest_env_uv_get_total_memory);
    reg_env!("uv_get_available_memory", guest_env_uv_get_available_memory);
    reg_env!(
        "uv_get_constrained_memory",
        guest_env_uv_get_constrained_memory
    );
}
