set(NAPI_JSC_GTEST_ROOT "${PROJECT_ROOT}/deps/googletest")
include(GoogleTest)
option(NAPI_JSC_BUILD_NODE_API_TESTS "Build node-api fixture tests in napi-jsc" OFF)

if(NAPI_JSC_LIBRARY)
  add_library(napi_jsc_gtest STATIC
    "${NAPI_JSC_GTEST_ROOT}/src/gtest-all.cc"
    "${NAPI_JSC_GTEST_ROOT}/src/gtest_main.cc"
  )

  target_include_directories(napi_jsc_gtest
    PRIVATE
      "${NAPI_JSC_GTEST_ROOT}"
      "${NAPI_JSC_GTEST_ROOT}/include"
      "${NAPI_TESTS_ROOT}/js-native-api"
      "${NAPI_TESTS_ROOT}/node-api"
      "${CMAKE_CURRENT_SOURCE_DIR}/runners"
      "${NAPI_INCLUDE_ROOT}"
      "${NAPI_JSC_INCLUDE_DIR}"
  )

  add_executable(napi_jsc_runtime_probe
    runners/jsc_runtime_probe.cc
  )
  target_include_directories(napi_jsc_runtime_probe
    PRIVATE
      "${NAPI_INCLUDE_ROOT}"
      "${NAPI_JSC_ROOT}/src"
      "${NAPI_JSC_INCLUDE_DIR}"
  )
  target_link_libraries(napi_jsc_runtime_probe
    PRIVATE
      napi_jsc
  )
  get_filename_component(NAPI_JSC_LIB_DIR "${NAPI_JSC_LIBRARY}" DIRECTORY)
  set_target_properties(napi_jsc_runtime_probe PROPERTIES
    BUILD_RPATH "${NAPI_JSC_LIB_DIR}"
    INSTALL_RPATH "${NAPI_JSC_LIB_DIR}"
  )
  add_custom_target(napi_jsc_tests DEPENDS napi_jsc_runtime_probe)

  function(add_fixture_test target_name)
    if(NOT NAPI_JSC_BUILD_NODE_API_TESTS AND
       target_name MATCHES "^napi_jsc_test_(20_node_general|21_general|22_node_exception|23_instance_data_ref_then_set|24_instance_data_set_then_ref|28_node_instance_data|38_finalizer|42_node_async|43_node_hello_world|44_node_async_cleanup_hook|45_node_async_context|46_node_buffer|47_node_buffer_finalizer|48_node_callback_scope|49_node_cleanup_hook|50_node_env_teardown_gc|51_node_fatal|52_node_fatal_exception|53_node_init_order|54_node_make_callback|55_node_make_callback_recurse|56_node_reference_by_api_v9|57_node_reference_by_api_v10|58_node_threadsafe_function|59_node_threadsafe_function_shutdown|60_node_uv_loop|61_node_uv_threadpool_size|62_node_worker_buffer_callback|63_node_worker_terminate|64_node_worker_terminate_finalization)$")
      return()
    endif()

    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name}
      PRIVATE
        "${NAPI_JSC_GTEST_ROOT}"
        "${NAPI_JSC_GTEST_ROOT}/include"
        "${NAPI_TESTS_ROOT}/js-native-api"
        "${NAPI_TESTS_ROOT}/node-api"
        "${CMAKE_CURRENT_SOURCE_DIR}/runners"
        "${NAPI_INCLUDE_ROOT}"
        "${NAPI_JSC_INCLUDE_DIR}"
    )
    target_link_libraries(${target_name}
      PRIVATE
        napi_jsc_gtest
        napi_jsc
        "${NAPI_JSC_LIBRARY}"
        $<TARGET_NAME_IF_EXISTS:edge_environment_core>
        $<TARGET_NAME_IF_EXISTS:uv_a>
    )
    target_compile_definitions(${target_name}
      PRIVATE
        NAPI_JSC_NODE_ROOT_PATH="${NAPI_JSC_NODE_ROOT}"
        NAPI_JSC_ROOT_PATH="${NAPI_JSC_ROOT}"
        NAPI_TESTS_ROOT_PATH="${NAPI_TESTS_ROOT}"
        NODE_API_EXPERIMENTAL_BASIC_ENV_OPT_OUT
    )
    target_compile_options(${target_name}
      PRIVATE
        "-DNAPI_MODULE(modname,regfunc)="
        "-DNODE_GYP_MODULE_NAME=napi_test_module"
        "-Wno-incompatible-function-pointer-types"
    )
    if(NAPI_JSC_DEFINES)
      target_compile_definitions(${target_name} PRIVATE ${NAPI_JSC_DEFINES})
    endif()
    if(NAPI_JSC_EXTRA_LIBS)
      target_link_libraries(${target_name} PRIVATE ${NAPI_JSC_EXTRA_LIBS})
    endif()
    get_filename_component(NAPI_JSC_LIB_DIR "${NAPI_JSC_LIBRARY}" DIRECTORY)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "${NAPI_JSC_LIB_DIR}"
      INSTALL_RPATH "${NAPI_JSC_LIB_DIR}"
    )
    add_dependencies(napi_jsc_tests ${target_name})
    gtest_discover_tests(
      ${target_name}
      DISCOVERY_TIMEOUT 30
      TEST_PREFIX "napi_jsc.${target_name}."
    )
  endfunction()

  add_fixture_test(napi_jsc_test_2
    runners/test_2_function_arguments.cc
    ${NAPI_TESTS_ROOT}/js-native-api/2_function_arguments/2_function_arguments.c
  )
  add_fixture_test(napi_jsc_test_3
    runners/test_3_callbacks.cc
    ${NAPI_TESTS_ROOT}/js-native-api/3_callbacks/3_callbacks.c
  )
  add_fixture_test(napi_jsc_test_4
    runners/test_4_object_factory.cc
    ${NAPI_TESTS_ROOT}/js-native-api/4_object_factory/4_object_factory.c
  )
  add_fixture_test(napi_jsc_test_5
    runners/test_5_function_factory.cc
    ${NAPI_TESTS_ROOT}/js-native-api/5_function_factory/5_function_factory.c
  )
  add_fixture_test(napi_jsc_test_6
    runners/test_6_object_wrap.cc
    ${NAPI_TESTS_ROOT}/js-native-api/6_object_wrap/myobject.cc
  )
  add_fixture_test(napi_jsc_test_7
    runners/test_7_factory_wrap.cc
    ${NAPI_TESTS_ROOT}/js-native-api/7_factory_wrap/7_factory_wrap.cc
    ${NAPI_TESTS_ROOT}/js-native-api/7_factory_wrap/myobject.cc
  )
  add_fixture_test(napi_jsc_test_8
    runners/test_8_passing_wrapped.cc
    ${NAPI_TESTS_ROOT}/js-native-api/8_passing_wrapped/8_passing_wrapped.cc
    ${NAPI_TESTS_ROOT}/js-native-api/8_passing_wrapped/myobject.cc
  )
  add_fixture_test(napi_jsc_test_9_array
    runners/test_9_array.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_array/test_array.c
  )
  add_fixture_test(napi_jsc_test_10_number
    runners/test_10_number.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_number/test_number.c
    ${NAPI_TESTS_ROOT}/js-native-api/test_number/test_null.c
  )
  add_fixture_test(napi_jsc_test_11_constructor
    runners/test_11_constructor.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_constructor/test_constructor.c
    ${NAPI_TESTS_ROOT}/js-native-api/test_constructor/test_null.c
  )
  add_fixture_test(napi_jsc_test_12_error
    runners/test_12_error.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_error/test_error.c
  )
  add_fixture_test(napi_jsc_test_13_symbol
    runners/test_13_symbol.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_symbol/test_symbol.c
  )
  add_fixture_test(napi_jsc_test_14_exception
    runners/test_14_exception.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_exception/test_exception.c
  )
  add_fixture_test(napi_jsc_test_15_function
    runners/test_15_function.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_function/test_function.c
  )
  add_fixture_test(napi_jsc_test_16_reference
    runners/test_16_reference.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_reference/test_reference.c
  )
  add_fixture_test(napi_jsc_test_17_string
    runners/test_17_string.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_string/test_string.c
    ${NAPI_TESTS_ROOT}/js-native-api/test_string/test_null.c
  )
  target_compile_definitions(napi_jsc_test_17_string PRIVATE NAPI_VERSION=10)
  add_fixture_test(napi_jsc_test_18_conversions
    runners/test_18_conversions.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_conversions/test_conversions.c
    ${NAPI_TESTS_ROOT}/js-native-api/test_conversions/test_null.c
  )
  add_fixture_test(napi_jsc_test_19_properties
    runners/test_19_properties.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_properties/test_properties.c
  )
  add_fixture_test(napi_jsc_test_20_node_general
    runners/test_20_node_general.cc
    ${NAPI_TESTS_ROOT}/node-api/test_general/test_general.c
  )
  add_fixture_test(napi_jsc_test_21_general
    runners/test_21_general.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_general/test_general.c
  )
  if(TARGET napi_jsc_test_21_general)
    target_compile_definitions(napi_jsc_test_21_general PRIVATE NAPI_EXPERIMENTAL)
  endif()
  add_fixture_test(napi_jsc_test_22_node_exception
    runners/test_22_node_exception.cc
    ${NAPI_TESTS_ROOT}/node-api/test_exception/test_exception.c
  )
  add_fixture_test(napi_jsc_test_23_instance_data_ref_then_set
    runners/test_23_instance_data_ref_then_set.cc
    ${NAPI_TESTS_ROOT}/node-api/test_instance_data/addon.c
    ${NAPI_TESTS_ROOT}/node-api/test_instance_data/test_ref_then_set.c
  )
  add_fixture_test(napi_jsc_test_24_instance_data_set_then_ref
    runners/test_24_instance_data_set_then_ref.cc
    ${NAPI_TESTS_ROOT}/node-api/test_instance_data/addon.c
    ${NAPI_TESTS_ROOT}/node-api/test_instance_data/test_set_then_ref.c
  )
  add_fixture_test(napi_jsc_test_25_object_exceptions
    runners/test_25_object_exceptions.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_object/test_exceptions.c
  )
  add_fixture_test(napi_jsc_test_26_object_null
    runners/test_26_object_null.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_object/test_null.c
  )
  add_fixture_test(napi_jsc_test_27_object
    runners/test_27_object.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_object/test_object.c
    ${NAPI_TESTS_ROOT}/js-native-api/test_object/test_null.c
  )
  target_compile_definitions(napi_jsc_test_27_object PRIVATE NAPI_EXPERIMENTAL)
  add_fixture_test(napi_jsc_test_28_node_instance_data
    runners/test_28_node_instance_data.cc
    ${NAPI_TESTS_ROOT}/node-api/test_instance_data/test_instance_data.c
  )
  add_fixture_test(napi_jsc_test_29_bigint
    runners/test_29_bigint.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_bigint/test_bigint.c
  )
  add_fixture_test(napi_jsc_test_30_date
    runners/test_30_date.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_date/test_date.c
  )
  add_fixture_test(napi_jsc_test_31_dataview
    runners/test_31_dataview.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_dataview/test_dataview.c
  )
  target_compile_definitions(napi_jsc_test_31_dataview PRIVATE NAPI_EXPERIMENTAL NODE_API_EXPERIMENTAL_NO_WARNING)
  add_fixture_test(napi_jsc_test_32_sharedarraybuffer
    runners/test_32_sharedarraybuffer.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_sharedarraybuffer/test_sharedarraybuffer.c
  )
  target_compile_definitions(napi_jsc_test_32_sharedarraybuffer PRIVATE NAPI_EXPERIMENTAL NODE_API_EXPERIMENTAL_NO_WARNING)
  add_fixture_test(napi_jsc_test_33_typedarray
    runners/test_33_typedarray.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_typedarray/test_typedarray.c
  )
  add_fixture_test(napi_jsc_test_34_new_target
    runners/test_34_new_target.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_new_target/test_new_target.c
  )
  add_fixture_test(napi_jsc_test_35_promise
    runners/test_35_promise.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_promise/test_promise.c
  )
  add_fixture_test(napi_jsc_test_36_handle_scope
    runners/test_36_handle_scope.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_handle_scope/test_handle_scope.c
  )
  add_fixture_test(napi_jsc_test_37_reference_double_free
    runners/test_37_reference_double_free.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_reference_double_free/test_reference_double_free.c
  )
  add_fixture_test(napi_jsc_test_38_finalizer
    runners/test_38_finalizer.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_finalizer/test_finalizer.c
  )
  if(TARGET napi_jsc_test_38_finalizer)
    target_compile_definitions(napi_jsc_test_38_finalizer PRIVATE NAPI_EXPERIMENTAL NODE_API_EXPERIMENTAL_NO_WARNING)
  endif()
  add_fixture_test(napi_jsc_test_39_cannot_run_js
    runners/test_39_cannot_run_js.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_cannot_run_js/test_cannot_run_js.c
  )
  target_compile_definitions(napi_jsc_test_39_cannot_run_js PRIVATE NAPI_VERSION=10)
  add_fixture_test(napi_jsc_test_40_pending_exception
    runners/test_40_pending_exception.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_cannot_run_js/test_cannot_run_js.c
  )
  target_compile_definitions(napi_jsc_test_40_pending_exception PRIVATE NAPI_VERSION=9)
  add_fixture_test(napi_jsc_test_41_instance_data
    runners/test_41_instance_data.cc
    ${NAPI_TESTS_ROOT}/js-native-api/test_instance_data/test_instance_data.c
  )
  add_fixture_test(napi_jsc_test_42_node_async
    runners/test_42_node_async.cc
    ${NAPI_TESTS_ROOT}/node-api/test_async/test_async.c
  )
  add_fixture_test(napi_jsc_test_43_node_hello_world
    runners/test_43_node_hello_world.cc
    ${NAPI_TESTS_ROOT}/node-api/1_hello_world/binding.c
  )
  add_fixture_test(napi_jsc_test_44_node_async_cleanup_hook
    runners/test_44_node_async_cleanup_hook.cc
    ${NAPI_TESTS_ROOT}/node-api/test_async_cleanup_hook/binding.c
  )
  add_fixture_test(napi_jsc_test_45_node_async_context
    runners/test_45_node_async_context.cc
    ${NAPI_TESTS_ROOT}/node-api/test_async_context/binding.c
  )
  add_fixture_test(napi_jsc_test_46_node_buffer
    runners/test_46_node_buffer.cc
    ${NAPI_TESTS_ROOT}/node-api/test_buffer/test_buffer.c
  )
  if(TARGET napi_jsc_test_46_node_buffer)
    target_compile_definitions(napi_jsc_test_46_node_buffer PRIVATE NAPI_VERSION=10)
  endif()
  add_fixture_test(napi_jsc_test_47_node_buffer_finalizer
    runners/test_47_node_buffer_finalizer.cc
    ${NAPI_TESTS_ROOT}/node-api/test_buffer/test_finalizer.c
  )
  add_fixture_test(napi_jsc_test_48_node_callback_scope
    runners/test_48_node_callback_scope.cc
    ${NAPI_TESTS_ROOT}/node-api/test_callback_scope/binding.c
  )
  add_fixture_test(napi_jsc_test_49_node_cleanup_hook
    runners/test_49_node_cleanup_hook.cc
    ${NAPI_TESTS_ROOT}/node-api/test_cleanup_hook/binding.c
  )
  add_fixture_test(napi_jsc_test_50_node_env_teardown_gc
    runners/test_50_node_env_teardown_gc.cc
    ${NAPI_TESTS_ROOT}/node-api/test_env_teardown_gc/binding.c
  )
  add_fixture_test(napi_jsc_test_51_node_fatal
    runners/test_51_node_fatal.cc
    ${NAPI_TESTS_ROOT}/node-api/test_fatal/test_fatal.c
  )
  add_fixture_test(napi_jsc_test_52_node_fatal_exception
    runners/test_52_node_fatal_exception.cc
    ${NAPI_TESTS_ROOT}/node-api/test_fatal_exception/test_fatal_exception.c
  )
  add_fixture_test(napi_jsc_test_53_node_init_order
    runners/test_53_node_init_order.cc
    ${NAPI_TESTS_ROOT}/node-api/test_init_order/test_init_order.cc
  )
  add_fixture_test(napi_jsc_test_54_node_make_callback
    runners/test_54_node_make_callback.cc
    ${NAPI_TESTS_ROOT}/node-api/test_make_callback/binding.c
  )
  add_fixture_test(napi_jsc_test_55_node_make_callback_recurse
    runners/test_55_node_make_callback_recurse.cc
    ${NAPI_TESTS_ROOT}/node-api/test_make_callback_recurse/binding.c
  )
  add_fixture_test(napi_jsc_test_56_node_reference_by_api_v9
    runners/test_56_node_reference_by_api_v9.cc
    ${NAPI_TESTS_ROOT}/node-api/test_reference_by_node_api_version/test_reference_by_node_api_version.c
  )
  if(TARGET napi_jsc_test_56_node_reference_by_api_v9)
    target_compile_definitions(napi_jsc_test_56_node_reference_by_api_v9 PRIVATE NAPI_VERSION=9)
  endif()
  add_fixture_test(napi_jsc_test_57_node_reference_by_api_v10
    runners/test_57_node_reference_by_api_v10.cc
    ${NAPI_TESTS_ROOT}/node-api/test_reference_by_node_api_version/test_reference_by_node_api_version.c
  )
  if(TARGET napi_jsc_test_57_node_reference_by_api_v10)
    target_compile_definitions(napi_jsc_test_57_node_reference_by_api_v10 PRIVATE NAPI_VERSION=10)
  endif()
  add_fixture_test(napi_jsc_test_58_node_threadsafe_function
    runners/test_58_node_threadsafe_function.cc
    ${NAPI_TESTS_ROOT}/node-api/test_threadsafe_function/binding.c
  )
  add_fixture_test(napi_jsc_test_59_node_threadsafe_function_shutdown
    runners/test_59_node_threadsafe_function_shutdown.cc
    ${NAPI_TESTS_ROOT}/node-api/test_threadsafe_function_shutdown/binding.cc
  )
  add_fixture_test(napi_jsc_test_60_node_uv_loop
    runners/test_60_node_uv_loop.cc
    ${NAPI_TESTS_ROOT}/node-api/test_uv_loop/test_uv_loop.cc
  )
  add_fixture_test(napi_jsc_test_61_node_uv_threadpool_size
    runners/test_61_node_uv_threadpool_size.cc
    ${NAPI_TESTS_ROOT}/node-api/test_uv_threadpool_size/test_uv_threadpool_size.c
  )
  add_fixture_test(napi_jsc_test_62_node_worker_buffer_callback
    runners/test_62_node_worker_buffer_callback.cc
    ${NAPI_TESTS_ROOT}/node-api/test_worker_buffer_callback/test_worker_buffer_callback.c
  )
  add_fixture_test(napi_jsc_test_63_node_worker_terminate
    runners/test_63_node_worker_terminate.cc
    ${NAPI_TESTS_ROOT}/node-api/test_worker_terminate/test_worker_terminate.c
  )
  add_fixture_test(napi_jsc_test_64_node_worker_terminate_finalization
    runners/test_64_node_worker_terminate_finalization.cc
    ${NAPI_TESTS_ROOT}/node-api/test_worker_terminate_finalization/test_worker_terminate_finalization.c
  )
  add_fixture_test(napi_jsc_test_65_unofficial_contextify
    runners/test_65_unofficial_contextify.cc
  )
  add_fixture_test(napi_jsc_test_66_unofficial_unsupported
    runners/test_66_unofficial_unsupported.cc
  )
  add_fixture_test(napi_jsc_test_67_jsc_config_guard
    runners/test_67_jsc_config_guard.cc
  )
else()
  message(STATUS "Skipping napi_jsc_tier1_tests: set NAPI_JSC_LIBRARY to link JavaScriptCore")
endif()
