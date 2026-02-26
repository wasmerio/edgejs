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
    # Keep a stable path to the generated CTest manifest for this target.
    set(_gtest_tests_manifest "${CMAKE_CURRENT_BINARY_DIR}/${target_name}[1]_tests.cmake")
    gtest_discover_tests(
      ${target_name}
      DISCOVERY_TIMEOUT 30
      TEST_PREFIX "unode.${target_name}."
    )
    # CMake 3.31 can emit duplicated add_test entries for very large gtest lists.
    # Deduplicate generated manifest entries so each gtest runs once in ctest.
    add_custom_command(
      TARGET ${target_name}
      POST_BUILD
      COMMAND /usr/bin/env python3
              ${UNODE_ROOT}/tests/cmake/dedupe_gtest_manifest.py
              ${_gtest_tests_manifest}
      VERBATIM
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
