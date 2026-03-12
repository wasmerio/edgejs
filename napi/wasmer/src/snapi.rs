// ============================================================
// C++ bridge FFI declarations (from napi_bridge_init.cc)
// ============================================================

unsafe extern "C" {
    pub fn snapi_bridge_init() -> i32;
    pub fn snapi_bridge_unofficial_create_env(
        module_api_version: i32,
        env_out: *mut u32,
        scope_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_release_env(scope_handle: u32) -> i32;
    pub fn snapi_bridge_unofficial_process_microtasks(env_handle: u32) -> i32;
    pub fn snapi_bridge_unofficial_request_gc_for_testing(env_handle: u32) -> i32;
    pub fn snapi_bridge_unofficial_get_promise_details(
        env_handle: u32,
        promise_id: u32,
        state_out: *mut i32,
        result_out: *mut u32,
        has_result_out: *mut i32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_get_proxy_details(
        env_handle: u32,
        proxy_id: u32,
        target_out: *mut u32,
        handler_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_preview_entries(
        env_handle: u32,
        value_id: u32,
        entries_out: *mut u32,
        is_key_value_out: *mut i32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_get_call_sites(
        env_handle: u32,
        frames: u32,
        callsites_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_get_caller_location(
        env_handle: u32,
        location_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_arraybuffer_view_has_buffer(
        env_handle: u32,
        value_id: u32,
        result_out: *mut i32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_get_constructor_name(
        env_handle: u32,
        value_id: u32,
        name_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_create_private_symbol(
        env_handle: u32,
        str_ptr: *const i8,
        wasm_length: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_get_continuation_preserved_embedder_data(
        env_handle: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_set_continuation_preserved_embedder_data(
        env_handle: u32,
        value_id: u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_set_enqueue_foreground_task_callback(env_handle: u32) -> i32;
    pub fn snapi_bridge_unofficial_set_fatal_error_callbacks(
        env_handle: u32,
        fatal_callback_id: u32,
        oom_callback_id: u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_terminate_execution(env_handle: u32) -> i32;
    pub fn snapi_bridge_unofficial_enqueue_microtask(env_handle: u32, callback_id: u32) -> i32;
    pub fn snapi_bridge_unofficial_set_promise_reject_callback(
        env_handle: u32,
        callback_id: u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_get_own_non_index_properties(
        env_handle: u32,
        value_id: u32,
        filter_bits: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_get_process_memory_info(
        env_handle: u32,
        heap_total_out: *mut f64,
        heap_used_out: *mut f64,
        external_out: *mut f64,
        array_buffers_out: *mut f64,
    ) -> i32;
    pub fn snapi_bridge_unofficial_structured_clone(
        env_handle: u32,
        value_id: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_notify_datetime_configuration_change(env_handle: u32) -> i32;
    pub fn snapi_bridge_unofficial_create_serdes_binding(env_handle: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_unofficial_contextify_contains_module_syntax(
        env_handle: u32,
        code_id: u32,
        filename_id: u32,
        resource_name_id: u32,
        cjs_var_in_scope: i32,
        result_out: *mut i32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_contextify_make_context(
        env_handle: u32,
        sandbox_or_symbol_id: u32,
        name_id: u32,
        origin_id: u32,
        allow_code_gen_strings: i32,
        allow_code_gen_wasm: i32,
        own_microtask_queue: i32,
        host_defined_option_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_contextify_run_script(
        env_handle: u32,
        sandbox_or_null_id: u32,
        source_id: u32,
        filename_id: u32,
        line_offset: i32,
        column_offset: i32,
        timeout: i64,
        display_errors: i32,
        break_on_sigint: i32,
        break_on_first_line: i32,
        host_defined_option_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_contextify_dispose_context(
        env_handle: u32,
        sandbox_or_context_global_id: u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_contextify_compile_function(
        env_handle: u32,
        code_id: u32,
        filename_id: u32,
        line_offset: i32,
        column_offset: i32,
        cached_data_id: u32,
        produce_cached_data: i32,
        parsing_context_id: u32,
        context_extensions_id: u32,
        params_id: u32,
        host_defined_option_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_contextify_compile_function_for_cjs_loader(
        env_handle: u32,
        code_id: u32,
        filename_id: u32,
        is_sea_main: i32,
        should_detect_module: i32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_contextify_create_cached_data(
        env_handle: u32,
        code_id: u32,
        filename_id: u32,
        line_offset: i32,
        column_offset: i32,
        host_defined_option_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_create_source_text(
        env_handle: u32,
        wrapper_id: u32,
        url_id: u32,
        context_id: u32,
        source_id: u32,
        line_offset: i32,
        column_offset: i32,
        cached_data_or_id: u32,
        handle_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_create_synthetic(
        env_handle: u32,
        wrapper_id: u32,
        url_id: u32,
        context_id: u32,
        export_names_id: u32,
        synthetic_eval_steps_id: u32,
        handle_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_destroy(env_handle: u32, handle_id: u32) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_get_module_requests(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_link(
        env_handle: u32,
        handle_id: u32,
        count: u32,
        linked_handle_ids: *const u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_instantiate(env_handle: u32, handle_id: u32) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_evaluate(
        env_handle: u32,
        handle_id: u32,
        timeout: i64,
        break_on_sigint: i32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_evaluate_sync(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_get_namespace(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_get_status(
        env_handle: u32,
        handle_id: u32,
        status_out: *mut i32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_get_error(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_has_top_level_await(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut i32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_has_async_graph(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut i32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_set_export(
        env_handle: u32,
        handle_id: u32,
        export_name_id: u32,
        export_value_id: u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_set_module_source_object(
        env_handle: u32,
        handle_id: u32,
        source_object_id: u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_get_module_source_object(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_create_cached_data(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_set_import_module_dynamically_callback(
        env_handle: u32,
        callback_id: u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_set_initialize_import_meta_object_callback(
        env_handle: u32,
        callback_id: u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_import_module_dynamically(
        env_handle: u32,
        argc: u32,
        argv_ids: *const u32,
        result_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_unofficial_module_wrap_create_required_module_facade(
        env_handle: u32,
        handle_id: u32,
        result_out: *mut u32,
    ) -> i32;
    // Value creation
    pub fn snapi_bridge_get_undefined(out_id: *mut u32) -> i32;
    pub fn snapi_bridge_get_null(out_id: *mut u32) -> i32;
    pub fn snapi_bridge_get_boolean(value: i32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_get_global(out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_string_utf8(
        str_ptr: *const i8,
        wasm_length: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_create_string_latin1(
        str_ptr: *const i8,
        wasm_length: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_create_int32(value: i32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_uint32(value: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_double(value: f64, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_int64(value: i64, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_object(out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_array(out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_array_with_length(length: u32, out_id: *mut u32) -> i32;
    // Value reading
    pub fn snapi_bridge_get_value_string_utf8(
        id: u32,
        buf: *mut i8,
        bufsize: usize,
        result: *mut usize,
    ) -> i32;
    pub fn snapi_bridge_get_value_string_latin1(
        id: u32,
        buf: *mut i8,
        bufsize: usize,
        result: *mut usize,
    ) -> i32;
    pub fn snapi_bridge_get_value_int32(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_get_value_uint32(id: u32, result: *mut u32) -> i32;
    pub fn snapi_bridge_get_value_double(id: u32, result: *mut f64) -> i32;
    pub fn snapi_bridge_get_value_int64(id: u32, result: *mut i64) -> i32;
    pub fn snapi_bridge_get_value_bool(id: u32, result: *mut i32) -> i32;
    // Type checking
    pub fn snapi_bridge_typeof(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_is_array(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_is_error(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_is_arraybuffer(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_is_typedarray(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_is_dataview(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_is_date(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_is_promise(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_instanceof(obj_id: u32, ctor_id: u32, result: *mut i32) -> i32;
    // Coercion
    pub fn snapi_bridge_coerce_to_bool(id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_coerce_to_number(id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_coerce_to_string(id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_coerce_to_object(id: u32, out_id: *mut u32) -> i32;
    // Object operations
    pub fn snapi_bridge_set_property(obj_id: u32, key_id: u32, val_id: u32) -> i32;
    pub fn snapi_bridge_get_property(obj_id: u32, key_id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_has_property(obj_id: u32, key_id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_has_own_property(obj_id: u32, key_id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_delete_property(obj_id: u32, key_id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_set_named_property(obj_id: u32, name: *const i8, val_id: u32) -> i32;
    pub fn snapi_bridge_get_named_property(obj_id: u32, name: *const i8, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_has_named_property(obj_id: u32, name: *const i8, result: *mut i32) -> i32;
    pub fn snapi_bridge_set_element(obj_id: u32, index: u32, val_id: u32) -> i32;
    pub fn snapi_bridge_get_element(obj_id: u32, index: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_has_element(obj_id: u32, index: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_delete_element(obj_id: u32, index: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_get_array_length(arr_id: u32, result: *mut u32) -> i32;
    pub fn snapi_bridge_get_property_names(obj_id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_get_all_property_names(
        obj_id: u32,
        mode: i32,
        filter: i32,
        conversion: i32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_get_prototype(obj_id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_object_freeze(obj_id: u32) -> i32;
    pub fn snapi_bridge_object_seal(obj_id: u32) -> i32;
    // Comparison
    pub fn snapi_bridge_strict_equals(a_id: u32, b_id: u32, result: *mut i32) -> i32;
    // Error handling
    pub fn snapi_bridge_create_error(code_id: u32, msg_id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_type_error(code_id: u32, msg_id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_range_error(code_id: u32, msg_id: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_throw(error_id: u32) -> i32;
    pub fn snapi_bridge_throw_error(code: *const i8, msg: *const i8) -> i32;
    pub fn snapi_bridge_throw_type_error(code: *const i8, msg: *const i8) -> i32;
    pub fn snapi_bridge_throw_range_error(code: *const i8, msg: *const i8) -> i32;
    pub fn snapi_bridge_is_exception_pending(result: *mut i32) -> i32;
    pub fn snapi_bridge_get_and_clear_last_exception(out_id: *mut u32) -> i32;
    // Symbol
    pub fn snapi_bridge_create_symbol(description_id: u32, out_id: *mut u32) -> i32;
    // BigInt
    pub fn snapi_bridge_create_bigint_int64(value: i64, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_bigint_uint64(value: u64, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_get_value_bigint_int64(id: u32, value: *mut i64, lossless: *mut i32)
        -> i32;
    pub fn snapi_bridge_get_value_bigint_uint64(
        id: u32,
        value: *mut u64,
        lossless: *mut i32,
    ) -> i32;
    // Date
    pub fn snapi_bridge_create_date(time: f64, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_get_date_value(id: u32, result: *mut f64) -> i32;
    // Promise
    pub fn snapi_bridge_create_promise(deferred_out: *mut u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_resolve_deferred(deferred_id: u32, value_id: u32) -> i32;
    pub fn snapi_bridge_reject_deferred(deferred_id: u32, value_id: u32) -> i32;
    // ArrayBuffer
    pub fn snapi_bridge_create_arraybuffer(byte_length: u32, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_external_arraybuffer(
        data_addr: u64,
        byte_length: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_create_external_buffer(
        data_addr: u64,
        byte_length: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_get_arraybuffer_info(
        id: u32,
        data_out: *mut u64,
        byte_length: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_detach_arraybuffer(id: u32) -> i32;
    pub fn snapi_bridge_is_detached_arraybuffer(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_is_sharedarraybuffer(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_create_sharedarraybuffer(
        byte_length: u32,
        data_out: *mut u64,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_node_api_set_prototype(object_id: u32, prototype_id: u32) -> i32;
    // TypedArray
    pub fn snapi_bridge_create_typedarray(
        typ: i32,
        length: u32,
        arraybuffer_id: u32,
        byte_offset: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_get_typedarray_info(
        id: u32,
        type_out: *mut i32,
        length_out: *mut u32,
        data_out: *mut u64,
        arraybuffer_out: *mut u32,
        byte_offset_out: *mut u32,
    ) -> i32;
    // DataView
    pub fn snapi_bridge_create_dataview(
        byte_length: u32,
        arraybuffer_id: u32,
        byte_offset: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_get_dataview_info(
        id: u32,
        byte_length_out: *mut u32,
        data_out: *mut u64,
        arraybuffer_out: *mut u32,
        byte_offset_out: *mut u32,
    ) -> i32;
    // External
    pub fn snapi_bridge_create_external(data_val: u64, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_get_value_external(id: u32, data_out: *mut u64) -> i32;
    // References
    pub fn snapi_bridge_create_reference(
        value_id: u32,
        initial_refcount: u32,
        ref_out: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_delete_reference(ref_id: u32) -> i32;
    pub fn snapi_bridge_reference_ref(ref_id: u32, result: *mut u32) -> i32;
    pub fn snapi_bridge_reference_unref(ref_id: u32, result: *mut u32) -> i32;
    pub fn snapi_bridge_get_reference_value(ref_id: u32, out_id: *mut u32) -> i32;
    // Handle scopes (escapable)
    pub fn snapi_bridge_open_escapable_handle_scope(scope_out: *mut u32) -> i32;
    pub fn snapi_bridge_close_escapable_handle_scope(scope_id: u32) -> i32;
    pub fn snapi_bridge_escape_handle(scope_id: u32, escapee_id: u32, out_id: *mut u32) -> i32;
    // Type tagging
    pub fn snapi_bridge_type_tag_object(obj_id: u32, tag_lower: u64, tag_upper: u64) -> i32;
    pub fn snapi_bridge_check_object_type_tag(
        obj_id: u32,
        tag_lower: u64,
        tag_upper: u64,
        result: *mut i32,
    ) -> i32;
    // Function calling
    pub fn snapi_bridge_call_function(
        recv_id: u32,
        func_id: u32,
        argc: u32,
        argv_ids: *const u32,
        out_id: *mut u32,
    ) -> i32;
    // Script execution
    pub fn snapi_bridge_run_script(script_id: u32, out_value_id: *mut u32) -> i32;
    // UTF-16 strings
    pub fn snapi_bridge_create_string_utf16(
        str_ptr: *const u16,
        wasm_length: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_get_value_string_utf16(
        id: u32,
        buf: *mut u16,
        bufsize: usize,
        result: *mut usize,
    ) -> i32;
    // BigInt words
    pub fn snapi_bridge_create_bigint_words(
        sign_bit: i32,
        word_count: u32,
        words: *const u64,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_get_value_bigint_words(
        id: u32,
        sign_bit: *mut i32,
        word_count: *mut usize,
        words: *mut u64,
    ) -> i32;
    // Instance data
    pub fn snapi_bridge_set_instance_data(data_val: u64) -> i32;
    pub fn snapi_bridge_get_instance_data(data_out: *mut u64) -> i32;
    pub fn snapi_bridge_adjust_external_memory(change: i64, adjusted: *mut i64) -> i32;
    // Node Buffers
    pub fn snapi_bridge_create_buffer(length: u32, data_out: *mut u64, out_id: *mut u32) -> i32;
    pub fn snapi_bridge_create_buffer_copy(
        length: u32,
        src_data: *const u8,
        result_data_out: *mut u64,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_is_buffer(id: u32, result: *mut i32) -> i32;
    pub fn snapi_bridge_get_buffer_info(id: u32, data_out: *mut u64, length_out: *mut u32) -> i32;
    // Node version
    pub fn snapi_bridge_get_node_version(major: *mut u32, minor: *mut u32, patch: *mut u32) -> i32;
    // Object wrapping
    pub fn snapi_bridge_wrap(obj_id: u32, native_data: u64, ref_out: *mut u32) -> i32;
    pub fn snapi_bridge_unwrap(obj_id: u32, data_out: *mut u64) -> i32;
    pub fn snapi_bridge_remove_wrap(obj_id: u32, data_out: *mut u64) -> i32;
    pub fn snapi_bridge_add_finalizer(obj_id: u32, data_val: u64, ref_out: *mut u32) -> i32;
    // Constructor
    pub fn snapi_bridge_new_instance(
        ctor_id: u32,
        argc: u32,
        argv_ids: *const u32,
        out_id: *mut u32,
    ) -> i32;
    // Callback system
    pub fn snapi_bridge_create_function(
        utf8name: *const i8,
        name_len: u32,
        reg_id: u32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_alloc_cb_reg_id() -> u32;
    pub fn snapi_bridge_register_callback(reg_id: u32, wasm_fn_ptr: u32, data_val: u64);
    pub fn snapi_bridge_register_callback_pair(
        reg_id: u32,
        wasm_getter_fn_ptr: u32,
        wasm_setter_fn_ptr: u32,
        data_val: u64,
    );
    pub fn snapi_bridge_get_cb_info(
        argc_ptr: *mut u32,
        argv_out: *mut u32,
        max_argv: u32,
        this_out: *mut u32,
        data_out: *mut u64,
    ) -> i32;
    pub fn snapi_bridge_get_new_target(out_id: *mut u32) -> i32;
    // napi_define_class
    pub fn snapi_bridge_define_class(
        utf8name: *const i8,
        name_len: u32,
        ctor_reg_id: u32,
        prop_count: u32,
        prop_names: *const *const i8,
        prop_name_ids: *const u32,
        prop_types: *const u32,
        prop_value_ids: *const u32,
        prop_method_reg_ids: *const u32,
        prop_getter_reg_ids: *const u32,
        prop_setter_reg_ids: *const u32,
        prop_attributes: *const i32,
        out_id: *mut u32,
    ) -> i32;
    pub fn snapi_bridge_define_properties(
        obj_id: u32,
        prop_count: u32,
        prop_names: *const *const i8,
        prop_name_ids: *const u32,
        prop_types: *const u32,
        prop_value_ids: *const u32,
        prop_method_reg_ids: *const u32,
        prop_getter_reg_ids: *const u32,
        prop_setter_reg_ids: *const u32,
        prop_attributes: *const i32,
    ) -> i32;
    // Cleanup
    #[allow(dead_code)]
    pub fn snapi_bridge_dispose();
}
