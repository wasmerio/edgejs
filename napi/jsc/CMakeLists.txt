cmake_minimum_required(VERSION 3.20)
project(napi_jsc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NAPI_JSC_BUILD_TESTS "Build napi-jsc tier1 tests" ON)
option(NAPI_JSC_BUILD_NODE_API_TESTS "Build node-api fixture tests in napi-jsc" OFF)
option(NAPI_JSC_REQUIRE_SHARED_ARRAYBUFFER
  "Fail env creation when SharedArrayBuffer is unavailable in the configured JavaScriptCore runtime"
  ON)

set(NAPI_JSC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include("${NAPI_JSC_ROOT}/cmake/NapiBuildCache.cmake")
napi_enable_sccache_if_available()
include("${NAPI_JSC_ROOT}/cmake/NapiEngine.cmake")
include("${NAPI_JSC_ROOT}/cmake/NapiJscResolver.cmake")
napi_engine_set_layout("${NAPI_JSC_ROOT}")
set(NAPI_JSC_NODE_ROOT_DEFAULT "${PROJECT_ROOT}/node")
if(DEFINED ENV{NAPI_JSC_NODE_ROOT})
  set(NAPI_JSC_NODE_ROOT_DEFAULT "$ENV{NAPI_JSC_NODE_ROOT}")
endif()
set(NAPI_JSC_NODE_ROOT
  "${NAPI_JSC_NODE_ROOT_DEFAULT}"
  CACHE PATH "Path to Node source root (for raw Node tests/fixtures)"
)
napi_jsc_resolve_configuration()

set(NAPI_JSC_STANDALONE_BUILD OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(NAPI_JSC_STANDALONE_BUILD ON)
endif()

set(NAPI_JSC_LIBUV_ROOT "${PROJECT_ROOT}/deps/uv")
if(CMAKE_SYSTEM_NAME MATCHES "WASI|WASIX" OR
   CMAKE_C_COMPILER MATCHES "wasixcc" OR
   CMAKE_CXX_COMPILER MATCHES "wasixcc\\+\\+")
  set(NAPI_JSC_LIBUV_ROOT "${PROJECT_ROOT}/deps/libuv-wasix")
endif()

if(NAPI_JSC_STANDALONE_BUILD)
  if(NOT TARGET uv_a)
    set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    add_subdirectory("${NAPI_JSC_LIBUV_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/deps/uv")
  endif()

  add_library(edge_environment_core STATIC
    "${PROJECT_ROOT}/src/edge_environment.cc"
  )
  target_include_directories(edge_environment_core
    PUBLIC
      "${PROJECT_ROOT}/src"
    PRIVATE
      "${NAPI_INCLUDE_ROOT}"
  )
  target_link_libraries(edge_environment_core PUBLIC uv_a)
  target_compile_definitions(edge_environment_core
    PUBLIC
      EDGE_BUNDLED_NAPI_PROVIDER=1
  )
endif()

add_library(napi_jsc STATIC
  src/bun_jsc_compat.cc
  src/jsc_runtime_config.cc
  src/js_native_api_jsc.cc
  src/unofficial_napi.cc
  src/unofficial_napi_contextify.cc
)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_custom_target(napi_jsc_symbol_coverage
    COMMAND
      "${Python3_EXECUTABLE}"
      "${NAPI_JSC_ROOT}/tools/check_symbol_coverage.py"
      "${NAPI_INCLUDE_ROOT}/js_native_api.h"
      "${NAPI_INCLUDE_ROOT}/node_api.h"
      "${NAPI_INCLUDE_ROOT}/unofficial_napi.h"
      --
      "${NAPI_JSC_ROOT}/src/js_native_api_jsc.cc"
      "${NAPI_JSC_ROOT}/src/unofficial_napi.cc"
      "${NAPI_JSC_ROOT}/src/unofficial_napi_contextify.cc"
      "${PROJECT_ROOT}/src/node_api.cc"
    DEPENDS
      "${NAPI_INCLUDE_ROOT}/js_native_api.h"
      "${NAPI_INCLUDE_ROOT}/node_api.h"
      "${NAPI_INCLUDE_ROOT}/unofficial_napi.h"
      "${NAPI_JSC_ROOT}/src/js_native_api_jsc.cc"
      "${NAPI_JSC_ROOT}/src/unofficial_napi.cc"
      "${NAPI_JSC_ROOT}/src/unofficial_napi_contextify.cc"
      "${PROJECT_ROOT}/src/node_api.cc"
      "${NAPI_JSC_ROOT}/tools/check_symbol_coverage.py"
    VERBATIM
  )
  add_dependencies(napi_jsc napi_jsc_symbol_coverage)
endif()

target_include_directories(napi_jsc
  PUBLIC
    "${NAPI_INCLUDE_ROOT}"
  PRIVATE
    "${NAPI_JSC_ROOT}/src"
    "${NAPI_JSC_INCLUDE_DIR}"
    "${NAPI_JSC_LIBUV_ROOT}/include"
)

target_compile_definitions(napi_jsc
  PUBLIC
    NAPI_EXTERN=
    NAPI_VERSION=10
    NAPI_EXPERIMENTAL
    EDGE_BUNDLED_NAPI_PROVIDER=1
    NAPI_JSC_REQUIRE_SHARED_ARRAYBUFFER=$<BOOL:${NAPI_JSC_REQUIRE_SHARED_ARRAYBUFFER}>
)
if(NAPI_JSC_DEFINES)
  target_compile_definitions(napi_jsc PRIVATE ${NAPI_JSC_DEFINES})
endif()
target_link_libraries(napi_jsc PUBLIC "${NAPI_JSC_LIBRARY}")
if(NAPI_JSC_EXTRA_LIBS)
  target_link_libraries(napi_jsc PUBLIC ${NAPI_JSC_EXTRA_LIBS})
endif()

if(NAPI_JSC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
