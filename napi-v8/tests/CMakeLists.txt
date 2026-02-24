set(NAPI_V8_V8_MONOLITH_LIB_DEFAULT "")
if(DEFINED ENV{NAPI_V8_V8_LIBRARY})
  set(NAPI_V8_V8_MONOLITH_LIB_DEFAULT "$ENV{NAPI_V8_V8_LIBRARY}")
elseif(DEFINED ENV{NAPI_V8_V8_MONOLITH_LIB})
  set(NAPI_V8_V8_MONOLITH_LIB_DEFAULT "$ENV{NAPI_V8_V8_MONOLITH_LIB}")
endif()
set(NAPI_V8_V8_MONOLITH_LIB
  "${NAPI_V8_V8_MONOLITH_LIB_DEFAULT}"
  CACHE FILEPATH "Path to V8 runtime library (monolith or dylib)"
)
set(NAPI_V8_V8_EXTRA_LIBS_DEFAULT "")
if(DEFINED ENV{NAPI_V8_V8_EXTRA_LIBS})
  set(NAPI_V8_V8_EXTRA_LIBS_DEFAULT "$ENV{NAPI_V8_V8_EXTRA_LIBS}")
endif()
set(NAPI_V8_V8_EXTRA_LIBS
  "${NAPI_V8_V8_EXTRA_LIBS_DEFAULT}"
  CACHE STRING "Semicolon-separated extra V8 libraries (e.g. libplatform/libbase)"
)
set(NAPI_V8_GTEST_ROOT "${NAPI_V8_NODE_ROOT}/deps/googletest")
set(NAPI_V8_UV_INCLUDE_DIR "${NAPI_V8_NODE_ROOT}/deps/uv/include")
include(GoogleTest)

if(NAPI_V8_V8_MONOLITH_LIB)
  add_library(napi_v8_gtest STATIC
    "${NAPI_V8_GTEST_ROOT}/src/gtest-all.cc"
    "${NAPI_V8_GTEST_ROOT}/src/gtest_main.cc"
  )

  target_include_directories(napi_v8_gtest
    PRIVATE
      "${NAPI_V8_GTEST_ROOT}"
      "${NAPI_V8_GTEST_ROOT}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/ported/js-native-api"
      "${CMAKE_CURRENT_SOURCE_DIR}/runners"
      "${NAPI_V8_ROOT}/include"
      "${NAPI_V8_V8_INCLUDE_DIR}"
  )

  function(add_fixture_test target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name}
      PRIVATE
        "${NAPI_V8_GTEST_ROOT}"
        "${NAPI_V8_GTEST_ROOT}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/ported/js-native-api"
        "${CMAKE_CURRENT_SOURCE_DIR}/runners"
        "${NAPI_V8_ROOT}/include"
        "${NAPI_V8_V8_INCLUDE_DIR}"
        "${NAPI_V8_UV_INCLUDE_DIR}"
    )
    target_link_libraries(${target_name}
      PRIVATE
        napi_v8_gtest
        napi_v8
        "${NAPI_V8_V8_MONOLITH_LIB}"
    )
    if(NAPI_V8_V8_DEFINES)
      target_compile_definitions(${target_name} PRIVATE ${NAPI_V8_V8_DEFINES})
    endif()
    if(NAPI_V8_V8_EXTRA_LIBS)
      target_link_libraries(${target_name} PRIVATE ${NAPI_V8_V8_EXTRA_LIBS})
    endif()
    get_filename_component(NAPI_V8_V8_LIB_DIR "${NAPI_V8_V8_MONOLITH_LIB}" DIRECTORY)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "${NAPI_V8_V8_LIB_DIR}"
      INSTALL_RPATH "${NAPI_V8_V8_LIB_DIR}"
    )
    gtest_discover_tests(
      ${target_name}
      DISCOVERY_TIMEOUT 30
      TEST_PREFIX "napi_v8.${target_name}."
    )
  endfunction()

  add_fixture_test(napi_v8_test_2
    runners/test_2_function_arguments.cc
    ported/js-native-api/2_function_arguments/2_function_arguments.c
  )
  add_fixture_test(napi_v8_test_3
    runners/test_3_callbacks.cc
    ported/js-native-api/3_callbacks/3_callbacks.c
  )
  add_fixture_test(napi_v8_test_4
    runners/test_4_object_factory.cc
    ported/js-native-api/4_object_factory/4_object_factory.c
  )
  add_fixture_test(napi_v8_test_5
    runners/test_5_function_factory.cc
    ported/js-native-api/5_function_factory/5_function_factory.c
  )
  add_fixture_test(napi_v8_test_6
    runners/test_6_object_wrap.cc
    ported/js-native-api/6_object_wrap/myobject.cc
  )
  add_fixture_test(napi_v8_test_7
    runners/test_7_factory_wrap.cc
    ported/js-native-api/7_factory_wrap/7_factory_wrap.cc
    ported/js-native-api/7_factory_wrap/myobject.cc
  )
  add_fixture_test(napi_v8_test_8
    runners/test_8_passing_wrapped.cc
    ported/js-native-api/8_passing_wrapped/8_passing_wrapped.cc
    ported/js-native-api/8_passing_wrapped/myobject.cc
  )
  add_fixture_test(napi_v8_test_9_array
    runners/test_9_array.cc
    ported/js-native-api/test_array/test_array.c
  )
  add_fixture_test(napi_v8_test_10_number
    runners/test_10_number.cc
    ported/js-native-api/test_number/test_number.c
    ported/js-native-api/test_number/test_null.c
  )
  add_fixture_test(napi_v8_test_11_constructor
    runners/test_11_constructor.cc
    ported/js-native-api/test_constructor/test_constructor.c
    ported/js-native-api/test_constructor/test_null.c
  )
  add_fixture_test(napi_v8_test_12_error
    runners/test_12_error.cc
    ported/js-native-api/test_error/test_error.c
  )
  add_fixture_test(napi_v8_test_13_symbol
    runners/test_13_symbol.cc
    ported/js-native-api/test_symbol/test_symbol.c
  )
  add_fixture_test(napi_v8_test_14_exception
    runners/test_14_exception.cc
    ported/js-native-api/test_exception/test_exception.c
  )
  add_fixture_test(napi_v8_test_15_function
    runners/test_15_function.cc
    ported/js-native-api/test_function/test_function.c
  )
  add_fixture_test(napi_v8_test_16_reference
    runners/test_16_reference.cc
    ported/js-native-api/test_reference/test_reference.c
  )
  add_fixture_test(napi_v8_test_17_string
    runners/test_17_string.cc
    ported/js-native-api/test_string/test_string.c
    ported/js-native-api/test_string/test_null.c
  )
  target_compile_definitions(napi_v8_test_17_string PRIVATE NAPI_VERSION=10)
  add_fixture_test(napi_v8_test_18_conversions
    runners/test_18_conversions.cc
    ported/js-native-api/test_conversions/test_conversions.c
    ported/js-native-api/test_conversions/test_null.c
  )
  add_fixture_test(napi_v8_test_19_properties
    runners/test_19_properties.cc
    ported/js-native-api/test_properties/test_properties.c
  )
  add_fixture_test(napi_v8_test_20_node_general
    runners/test_20_node_general.cc
    ported/node-api/test_general/test_general.c
  )
  add_fixture_test(napi_v8_test_21_general
    runners/test_21_general.cc
    ported/js-native-api/test_general/test_general.c
  )
  target_compile_definitions(napi_v8_test_21_general PRIVATE NAPI_EXPERIMENTAL)
  add_fixture_test(napi_v8_test_22_node_exception
    runners/test_22_node_exception.cc
    ported/node-api/test_exception/test_exception.c
  )
  add_fixture_test(napi_v8_test_23_instance_data_ref_then_set
    runners/test_23_instance_data_ref_then_set.cc
    ported/node-api/test_instance_data/addon.c
    ported/node-api/test_instance_data/test_ref_then_set.c
  )
  add_fixture_test(napi_v8_test_24_instance_data_set_then_ref
    runners/test_24_instance_data_set_then_ref.cc
    ported/node-api/test_instance_data/addon.c
    ported/node-api/test_instance_data/test_set_then_ref.c
  )
  add_fixture_test(napi_v8_test_25_object_exceptions
    runners/test_25_object_exceptions.cc
    ported/js-native-api/test_object/test_exceptions.c
  )
  add_fixture_test(napi_v8_test_26_object_null
    runners/test_26_object_null.cc
    ported/js-native-api/test_object/test_null.c
  )
  add_fixture_test(napi_v8_test_27_object
    runners/test_27_object.cc
    ported/js-native-api/test_object/test_object.c
    ported/js-native-api/test_object/test_null.c
  )
  target_compile_definitions(napi_v8_test_27_object PRIVATE NAPI_EXPERIMENTAL)
  add_fixture_test(napi_v8_test_28_node_instance_data
    runners/test_28_node_instance_data.cc
    ported/node-api/test_instance_data/test_instance_data.c
  )
  add_fixture_test(napi_v8_test_29_bigint
    runners/test_29_bigint.cc
    ported/js-native-api/test_bigint/test_bigint.c
  )
  add_fixture_test(napi_v8_test_30_date
    runners/test_30_date.cc
    ported/js-native-api/test_date/test_date.c
  )
  add_fixture_test(napi_v8_test_31_dataview
    runners/test_31_dataview.cc
    ported/js-native-api/test_dataview/test_dataview.c
  )
  target_compile_definitions(napi_v8_test_31_dataview PRIVATE NAPI_EXPERIMENTAL NODE_API_EXPERIMENTAL_NO_WARNING)
  add_fixture_test(napi_v8_test_32_sharedarraybuffer
    runners/test_32_sharedarraybuffer.cc
    ported/js-native-api/test_sharedarraybuffer/test_sharedarraybuffer.c
  )
  target_compile_definitions(napi_v8_test_32_sharedarraybuffer PRIVATE NAPI_EXPERIMENTAL NODE_API_EXPERIMENTAL_NO_WARNING)
  add_fixture_test(napi_v8_test_33_typedarray
    runners/test_33_typedarray.cc
    ported/js-native-api/test_typedarray/test_typedarray.c
  )
  add_fixture_test(napi_v8_test_34_new_target
    runners/test_34_new_target.cc
    ported/js-native-api/test_new_target/test_new_target.c
  )
  add_fixture_test(napi_v8_test_35_promise
    runners/test_35_promise.cc
    ported/js-native-api/test_promise/test_promise.c
  )
  add_fixture_test(napi_v8_test_36_handle_scope
    runners/test_36_handle_scope.cc
    ported/js-native-api/test_handle_scope/test_handle_scope.c
  )
  add_fixture_test(napi_v8_test_37_reference_double_free
    runners/test_37_reference_double_free.cc
    ported/js-native-api/test_reference_double_free/test_reference_double_free.c
  )
  add_fixture_test(napi_v8_test_38_finalizer
    runners/test_38_finalizer.cc
    ported/js-native-api/test_finalizer/test_finalizer.c
  )
  target_compile_definitions(napi_v8_test_38_finalizer PRIVATE NAPI_EXPERIMENTAL NODE_API_EXPERIMENTAL_NO_WARNING)
  find_library(NAPI_V8_UV_LIB NAMES uv libuv HINTS /opt/homebrew/lib /usr/local/lib)
  if(NAPI_V8_UV_LIB)
    target_link_libraries(napi_v8_test_28_node_instance_data PRIVATE "${NAPI_V8_UV_LIB}")
  endif()
else()
  message(STATUS "Skipping napi_v8_tier1_tests: set NAPI_V8_V8_MONOLITH_LIB to link V8 runtime")
endif()
