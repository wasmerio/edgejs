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
else()
  message(STATUS "Skipping napi_v8_tier1_tests: set NAPI_V8_V8_MONOLITH_LIB to link V8 runtime")
endif()
