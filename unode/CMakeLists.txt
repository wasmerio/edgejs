cmake_minimum_required(VERSION 3.20)
project(unode LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UNODE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NAPI_V8_ROOT "${UNODE_ROOT}/../napi-v8")
set(NAPI_V8_NODE_ROOT "${UNODE_ROOT}/../node")
set(UNODE_V8_MONOLITH_LIB_DEFAULT "")
if(DEFINED ENV{NAPI_V8_V8_LIBRARY})
  set(UNODE_V8_MONOLITH_LIB_DEFAULT "$ENV{NAPI_V8_V8_LIBRARY}")
elseif(DEFINED ENV{NAPI_V8_V8_MONOLITH_LIB})
  set(UNODE_V8_MONOLITH_LIB_DEFAULT "$ENV{NAPI_V8_V8_MONOLITH_LIB}")
endif()
set(UNODE_V8_MONOLITH_LIB
  "${UNODE_V8_MONOLITH_LIB_DEFAULT}"
  CACHE FILEPATH "Path to V8 runtime library (monolith or dylib)"
)
set(UNODE_V8_EXTRA_LIBS_DEFAULT "")
if(DEFINED ENV{NAPI_V8_V8_EXTRA_LIBS})
  set(UNODE_V8_EXTRA_LIBS_DEFAULT "$ENV{NAPI_V8_V8_EXTRA_LIBS}")
endif()
set(UNODE_V8_EXTRA_LIBS
  "${UNODE_V8_EXTRA_LIBS_DEFAULT}"
  CACHE STRING "Semicolon-separated extra V8 libraries (e.g. libplatform/libbase)"
)

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

set(NAPI_V8_UV_INCLUDE_DIR "${NAPI_V8_NODE_ROOT}/deps/uv/include")
find_library(UNODE_UV_LIB NAMES uv libuv HINTS /opt/homebrew/lib /usr/local/lib)

add_library(unode_node_api STATIC
  src/node_api.cc
  src/unode_node_api.cc
  src/unofficial_napi_testing_until_gc.cc
)

target_include_directories(unode_node_api
  PRIVATE
    "${NAPI_V8_ROOT}/src"
    "${NAPI_V8_ROOT}/include"
    "${NAPI_V8_V8_INCLUDE_DIR}"
    "${NAPI_V8_UV_INCLUDE_DIR}"
)

target_link_libraries(unode_node_api
  PUBLIC
    napi_v8
)
if(UNODE_UV_LIB)
  target_link_libraries(unode_node_api PRIVATE "${UNODE_UV_LIB}")
endif()

add_library(unode_runtime STATIC
  src/unode_runtime.cc
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
)

option(UNODE_BUILD_CLI "Build unode CLI binary" ON)
if(UNODE_BUILD_CLI)
  add_executable(unode
    "${NAPI_V8_ROOT}/src/unode_host_main.cc"
  )
  target_include_directories(unode
    PRIVATE
      "${UNODE_ROOT}/src"
      "${NAPI_V8_ROOT}/include"
      "${NAPI_V8_V8_INCLUDE_DIR}"
  )
  target_link_libraries(unode
    PRIVATE
      unode_runtime
      napi_v8
      "${UNODE_V8_MONOLITH_LIB}"
  )
  if(UNODE_V8_EXTRA_LIBS)
    target_link_libraries(unode PRIVATE ${UNODE_V8_EXTRA_LIBS})
  endif()
  if(NAPI_V8_V8_DEFINES)
    target_compile_definitions(unode PRIVATE ${NAPI_V8_V8_DEFINES})
  endif()
  if(UNODE_V8_MONOLITH_LIB)
    get_filename_component(UNODE_V8_LIB_DIR "${UNODE_V8_MONOLITH_LIB}" DIRECTORY)
    set_target_properties(unode PROPERTIES
      BUILD_RPATH "${UNODE_V8_LIB_DIR}"
      INSTALL_RPATH "${UNODE_V8_LIB_DIR}"
    )
  endif()
endif()

enable_testing()
add_subdirectory(tests)
