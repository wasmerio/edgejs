include(GoogleTest)

set(NAPI_V8_GTEST_ROOT "${NAPI_V8_NODE_ROOT}/deps/googletest")
set(NAPI_V8_UV_INCLUDE_DIR "${NAPI_V8_NODE_ROOT}/deps/uv/include")

if(UNODE_V8_MONOLITH_LIB)
  add_library(unode_gtest STATIC
    "${NAPI_V8_GTEST_ROOT}/src/gtest-all.cc"
    "${NAPI_V8_GTEST_ROOT}/src/gtest_main.cc"
  )

  target_include_directories(unode_gtest
    PRIVATE
      "${NAPI_V8_GTEST_ROOT}"
      "${NAPI_V8_GTEST_ROOT}/include"
      "${UNODE_ROOT}/tests/runners"
      "${UNODE_ROOT}/src"
      "${NAPI_V8_ROOT}/include"
      "${NAPI_V8_V8_INCLUDE_DIR}"
  )

  function(add_unode_node_api_test target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name}
      PRIVATE
        "${NAPI_V8_GTEST_ROOT}"
        "${NAPI_V8_GTEST_ROOT}/include"
        "${UNODE_ROOT}/tests/runners"
        "${UNODE_ROOT}/src"
        "${NAPI_V8_ROOT}/include"
        "${NAPI_V8_V8_INCLUDE_DIR}"
        "${NAPI_V8_UV_INCLUDE_DIR}"
    )
    target_link_libraries(${target_name}
      PRIVATE
        unode_gtest
        napi_v8
        unode_node_api
        unode_runtime
        "${UNODE_V8_MONOLITH_LIB}"
    )
    target_compile_definitions(${target_name}
      PRIVATE
        NAPI_V8_NODE_ROOT_PATH="${NAPI_V8_NODE_ROOT}"
        NAPI_V8_ROOT_PATH="${UNODE_ROOT}"
    )
    if(NAPI_V8_V8_DEFINES)
      target_compile_definitions(${target_name} PRIVATE ${NAPI_V8_V8_DEFINES})
    endif()
    if(UNODE_V8_EXTRA_LIBS)
      target_link_libraries(${target_name} PRIVATE ${UNODE_V8_EXTRA_LIBS})
    endif()
    get_filename_component(NAPI_V8_V8_LIB_DIR "${UNODE_V8_MONOLITH_LIB}" DIRECTORY)
    set_target_properties(${target_name} PROPERTIES
      BUILD_RPATH "${NAPI_V8_V8_LIB_DIR}"
      INSTALL_RPATH "${NAPI_V8_V8_LIB_DIR}"
    )
    gtest_discover_tests(
      ${target_name}
      DISCOVERY_TIMEOUT 30
      TEST_PREFIX "unode.${target_name}."
    )
  endfunction()

  add_unode_node_api_test(unode_test_0_runtime_phase01
    "${UNODE_ROOT}/tests/runners/test_0_runtime_phase01.cc"
  )
else()
  message(STATUS "Skipping unode node-api tests: set NAPI_V8_V8_LIBRARY to link V8 runtime")
endif()
