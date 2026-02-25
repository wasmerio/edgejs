cmake_minimum_required(VERSION 3.20)
project(napi_v8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NAPI_V8_BUILD_TESTS "Build napi-v8 tier1 tests" ON)

set(NAPI_V8_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NAPI_V8_NODE_ROOT "${NAPI_V8_ROOT}/../node")
set(NAPI_V8_V8_INCLUDE_DIR_DEFAULT "${NAPI_V8_NODE_ROOT}/deps/v8/include")
if(DEFINED ENV{NAPI_V8_V8_INCLUDE_DIR})
  set(NAPI_V8_V8_INCLUDE_DIR_DEFAULT "$ENV{NAPI_V8_V8_INCLUDE_DIR}")
endif()
set(NAPI_V8_V8_INCLUDE_DIR
  "${NAPI_V8_V8_INCLUDE_DIR_DEFAULT}"
  CACHE PATH "Path to V8 headers"
)
set(NAPI_V8_V8_DEFINES_DEFAULT "")
if(DEFINED ENV{NAPI_V8_V8_DEFINES})
  set(NAPI_V8_V8_DEFINES_DEFAULT "$ENV{NAPI_V8_V8_DEFINES}")
endif()
set(NAPI_V8_V8_DEFINES
  "${NAPI_V8_V8_DEFINES_DEFAULT}"
  CACHE STRING "Semicolon-separated V8 embedder compile definitions"
)
set(NAPI_V8_UV_INCLUDE_DIR "${NAPI_V8_NODE_ROOT}/deps/uv/include")

add_library(napi_v8 STATIC
  src/js_native_api_v8.cc
  src/napi_v8_platform.cc
  src/unofficial_napi_v8_test_env.cc
  src/unofficial_napi_request_gc_for_testing.cc
)

find_library(NAPI_V8_UV_LIB NAMES uv libuv HINTS /opt/homebrew/lib /usr/local/lib)

target_include_directories(napi_v8
  PUBLIC
    "${NAPI_V8_ROOT}/include"
  PRIVATE
    "${NAPI_V8_ROOT}/src"
    "${NAPI_V8_V8_INCLUDE_DIR}"
    "${NAPI_V8_UV_INCLUDE_DIR}"
)

target_compile_definitions(napi_v8
  PUBLIC
    NAPI_EXTERN=
)
if(NAPI_V8_V8_DEFINES)
  target_compile_definitions(napi_v8 PRIVATE ${NAPI_V8_V8_DEFINES})
endif()
if(NAPI_V8_UV_LIB)
  target_link_libraries(napi_v8 PRIVATE "${NAPI_V8_UV_LIB}")
endif()

if(NAPI_V8_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
