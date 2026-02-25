include(GoogleTest)

set(NAPI_V8_GTEST_ROOT "${NAPI_V8_NODE_ROOT}/deps/googletest")

if(NAPI_V8_V8_LIBRARY)
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
    )
    target_link_libraries(${target_name}
      PRIVATE
        unode_gtest
        napi_v8
        unode_node_api
        unode_runtime
        unode_cli
    )
    target_compile_definitions(${target_name}
      PRIVATE
        NAPI_V8_NODE_ROOT_PATH="${NAPI_V8_NODE_ROOT}"
        NAPI_V8_ROOT_PATH="${UNODE_ROOT}"
    )
    if(NAPI_V8_V8_DEFINES)
      target_compile_definitions(${target_name} PRIVATE ${NAPI_V8_V8_DEFINES})
    endif()
    gtest_discover_tests(
      ${target_name}
      DISCOVERY_TIMEOUT 30
      TEST_PREFIX "unode.${target_name}."
    )
  endfunction()

  add_unode_node_api_test(unode_test_0_runtime_phase01
    "${UNODE_ROOT}/tests/runners/test_0_runtime_phase01.cc"
  )
  add_unode_node_api_test(unode_test_1_cli_phase01
    "${UNODE_ROOT}/tests/runners/test_1_cli_phase01.cc"
  )
  add_unode_node_api_test(unode_test_2_module_loader_phase02
    "${UNODE_ROOT}/tests/runners/test_2_module_loader_phase02.cc"
  )
  add_unode_node_api_test(unode_test_3_node_dropin_subset_phase02
    "${UNODE_ROOT}/tests/runners/test_3_node_dropin_subset_phase02.cc"
  )
else()
  message(STATUS "Skipping unode node-api tests: set NAPI_V8_V8_LIBRARY for napi-v8 runtime linkage")
endif()
