cmake_minimum_required(VERSION 3.20)
project(unode LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UNODE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NAPI_V8_ROOT "${UNODE_ROOT}/../napi-v8")
set(NAPI_V8_NODE_ROOT "${UNODE_ROOT}/../node")

# Hard gate: unode runtime/library sources must never directly reference V8.
file(GLOB_RECURSE UNODE_PURE_SRC_FILES
  "${UNODE_ROOT}/src/*.h"
  "${UNODE_ROOT}/src/*.cc"
  "${UNODE_ROOT}/src/*.c"
)
foreach(UNODE_CODE_FILE IN LISTS UNODE_PURE_SRC_FILES)
  file(READ "${UNODE_CODE_FILE}" UNODE_CODE_CONTENTS)
  if(UNODE_CODE_CONTENTS MATCHES "#include <v8\\.h>" OR
     UNODE_CODE_CONTENTS MATCHES "#include <libplatform/libplatform\\.h>" OR
     UNODE_CODE_CONTENTS MATCHES "v8::")
    message(FATAL_ERROR
      "V8 usage is forbidden by policy in unode/src. "
      "Found in: ${UNODE_CODE_FILE}")
  endif()
endforeach()

# Build napi-v8 runtime only; node-api logic is owned by unode.
set(NAPI_V8_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${NAPI_V8_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/napi-v8")

# Use Node's bundled libuv (same as Node) for consistent behavior (e.g. mkdtemp).
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory("${NAPI_V8_NODE_ROOT}/deps/uv" "${CMAKE_CURRENT_BINARY_DIR}/deps/uv")

add_library(unode_node_api STATIC
  src/node_api.cc
  src/unofficial_napi_testing_until_gc.cc
)

target_include_directories(unode_node_api
  PRIVATE
    "${NAPI_V8_ROOT}/include"
)

target_link_libraries(unode_node_api
  PUBLIC
    napi_v8
  PRIVATE
    uv_a
)

add_library(unode_runtime STATIC
  src/unode_runtime.cc
  src/unode_module_loader.cc
  src/unode_fs.cc
)
target_include_directories(unode_runtime
  PUBLIC
    "${UNODE_ROOT}/src"
  PRIVATE
    "${NAPI_V8_ROOT}/include"
)
target_link_libraries(unode_runtime
  PUBLIC
    napi_v8
    unode_node_api
  PRIVATE
    uv_a
)

add_library(unode_cli STATIC
  src/unode_cli.cc
)
target_include_directories(unode_cli
  PUBLIC
    "${UNODE_ROOT}/src"
  PRIVATE
    "${NAPI_V8_ROOT}/include"
)
target_link_libraries(unode_cli
  PUBLIC
    unode_runtime
)

option(UNODE_BUILD_CLI "Build unode CLI binary" ON)
if(UNODE_BUILD_CLI)
  add_executable(unode
    "${UNODE_ROOT}/src/unode_main.cc"
  )
  target_include_directories(unode
    PRIVATE
      "${UNODE_ROOT}/src"
      "${NAPI_V8_ROOT}/include"
  )
  target_link_libraries(unode
    PRIVATE
      unode_cli
  )
  if(NAPI_V8_V8_DEFINES)
    target_compile_definitions(unode PRIVATE ${NAPI_V8_V8_DEFINES})
  endif()
endif()

enable_testing()
add_subdirectory(tests)
