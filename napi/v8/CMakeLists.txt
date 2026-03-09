cmake_minimum_required(VERSION 3.20)
project(napi_v8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NAPI_V8_BUILD_TESTS "Build napi-v8 tier1 tests" ON)

set(NAPI_V8_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include("${NAPI_V8_ROOT}/cmake/NapiBuildCache.cmake")
napi_enable_sccache_if_available()
include("${NAPI_V8_ROOT}/cmake/NapiEngine.cmake")
include("${NAPI_V8_ROOT}/cmake/NapiV8Resolver.cmake")
napi_engine_set_layout("${NAPI_V8_ROOT}")
set(NAPI_V8_NODE_ROOT_DEFAULT "${PROJECT_ROOT}/node")
if(DEFINED ENV{NAPI_V8_NODE_ROOT})
  set(NAPI_V8_NODE_ROOT_DEFAULT "$ENV{NAPI_V8_NODE_ROOT}")
endif()
set(NAPI_V8_NODE_ROOT
  "${NAPI_V8_NODE_ROOT_DEFAULT}"
  CACHE PATH "Path to Node source root (for raw Node tests/fixtures)"
)
napi_v8_resolve_configuration()

add_library(napi_v8 STATIC
  src/js_native_api_v8.cc
  src/unofficial_napi.cc
  src/unofficial_napi_contextify.cc
  src/ubi_v8_platform.cc
)

target_include_directories(napi_v8
  PUBLIC
    "${NAPI_INCLUDE_ROOT}"
  PRIVATE
    "${NAPI_V8_ROOT}/src"
    "${NAPI_V8_INCLUDE_DIR}"
)

target_compile_definitions(napi_v8
  PUBLIC
    NAPI_EXTERN=
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(napi_v8 PRIVATE -fno-rtti)
endif()
if(NAPI_V8_DEFINES)
  target_compile_definitions(napi_v8 PRIVATE ${NAPI_V8_DEFINES})
endif()
if(NAPI_V8_LIBRARY)
  target_link_libraries(napi_v8 PUBLIC "${NAPI_V8_LIBRARY}")
endif()
if(NAPI_V8_EXTRA_LIBS)
  target_link_libraries(napi_v8 PUBLIC ${NAPI_V8_EXTRA_LIBS})
endif()

if(NAPI_V8_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
