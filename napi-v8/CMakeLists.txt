cmake_minimum_required(VERSION 3.20)
project(napi_v8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NAPI_V8_BUILD_TESTS "Build napi-v8 tier1 tests" ON)

set(NAPI_V8_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(PROJECT_ROOT "${NAPI_V8_ROOT}/../")
set(NAPI_V8_NODE_ROOT_DEFAULT "${PROJECT_ROOT}/node")
if(DEFINED ENV{NAPI_V8_NODE_ROOT})
  set(NAPI_V8_NODE_ROOT_DEFAULT "$ENV{NAPI_V8_NODE_ROOT}")
endif()
set(NAPI_V8_NODE_ROOT
  "${NAPI_V8_NODE_ROOT_DEFAULT}"
  CACHE PATH "Path to Node source root (for raw Node tests/fixtures)"
)
set(NAPI_V8_V8_INCLUDE_DIR_DEFAULT "${PROJECT_ROOT}/deps/v8/include")
if(DEFINED ENV{NAPI_V8_V8_INCLUDE_DIR})
  set(NAPI_V8_V8_INCLUDE_DIR_DEFAULT "$ENV{NAPI_V8_V8_INCLUDE_DIR}")
endif()
# Fallback: Homebrew V8 (e.g. /opt/homebrew/Cellar/v8/14.5.201.9) when no env/cache set
set(NAPI_V8_V8_LIBRARY_DEFAULT "")
set(NAPI_V8_V8_EXTRA_LIBS_DEFAULT "")
if(DEFINED ENV{NAPI_V8_V8_LIBRARY})
  set(NAPI_V8_V8_LIBRARY_DEFAULT "$ENV{NAPI_V8_V8_LIBRARY}")
elseif(DEFINED ENV{NAPI_V8_V8_MONOLITH_LIB})
  set(NAPI_V8_V8_LIBRARY_DEFAULT "$ENV{NAPI_V8_V8_MONOLITH_LIB}")
elseif(APPLE)
  foreach(NAPI_V8_HB_PREFIX "/opt/homebrew/Cellar/v8/14.5.201.9" "/opt/homebrew/opt/v8")
    if(EXISTS "${NAPI_V8_HB_PREFIX}/lib/libv8.dylib")
      set(NAPI_V8_V8_LIBRARY_DEFAULT "${NAPI_V8_HB_PREFIX}/lib/libv8.dylib")
      set(NAPI_V8_V8_INCLUDE_DIR_DEFAULT "${NAPI_V8_HB_PREFIX}/include")
      set(NAPI_V8_V8_EXTRA_LIBS_DEFAULT "${NAPI_V8_HB_PREFIX}/lib/libv8_libplatform.dylib;${NAPI_V8_HB_PREFIX}/lib/libv8_libbase.dylib")
      # Homebrew V8 is built with pointer compression and sandbox; embedder must match.
      set(NAPI_V8_V8_DEFINES_DEFAULT "V8_COMPRESS_POINTERS;V8_31BIT_SMIS_ON_64BIT_ARCH;V8_ENABLE_SANDBOX")
      message(STATUS "Using Homebrew V8: ${NAPI_V8_V8_LIBRARY_DEFAULT}")
      break()
    endif()
  endforeach()
endif()
set(NAPI_V8_V8_INCLUDE_DIR
  "${NAPI_V8_V8_INCLUDE_DIR_DEFAULT}"
  CACHE PATH "Path to V8 headers"
)
if(NOT DEFINED NAPI_V8_V8_DEFINES_DEFAULT)
  set(NAPI_V8_V8_DEFINES_DEFAULT "")
endif()
if(DEFINED ENV{NAPI_V8_V8_DEFINES})
  set(NAPI_V8_V8_DEFINES_DEFAULT "$ENV{NAPI_V8_V8_DEFINES}")
endif()
set(NAPI_V8_V8_DEFINES
  "${NAPI_V8_V8_DEFINES_DEFAULT}"
  CACHE STRING "Semicolon-separated V8 embedder compile definitions"
)
if(NAPI_V8_V8_LIBRARY_DEFAULT AND NAPI_V8_V8_DEFINES_DEFAULT)
  set(NAPI_V8_V8_DEFINES "${NAPI_V8_V8_DEFINES_DEFAULT}" CACHE STRING "Semicolon-separated V8 embedder compile definitions" FORCE)
elseif(NAPI_V8_V8_DEFINES STREQUAL "" AND NAPI_V8_V8_DEFINES_DEFAULT)
  set(NAPI_V8_V8_DEFINES "${NAPI_V8_V8_DEFINES_DEFAULT}" CACHE STRING "Semicolon-separated V8 embedder compile definitions" FORCE)
endif()
set(NAPI_V8_V8_LIBRARY
  "${NAPI_V8_V8_LIBRARY_DEFAULT}"
  CACHE FILEPATH "Path to V8 runtime library (monolith or dylib)"
)
if(NAPI_V8_V8_LIBRARY STREQUAL "" AND NAPI_V8_V8_LIBRARY_DEFAULT)
  set(NAPI_V8_V8_LIBRARY "${NAPI_V8_V8_LIBRARY_DEFAULT}" CACHE FILEPATH "Path to V8 runtime library (monolith or dylib)" FORCE)
endif()
if(NAPI_V8_V8_INCLUDE_DIR STREQUAL "${PROJECT_ROOT}/deps/v8/include" AND NAPI_V8_V8_LIBRARY_DEFAULT)
  get_filename_component(NAPI_V8_HB_PREFIX "${NAPI_V8_V8_LIBRARY_DEFAULT}" DIRECTORY)
  get_filename_component(NAPI_V8_HB_PREFIX "${NAPI_V8_HB_PREFIX}" DIRECTORY)
  set(NAPI_V8_V8_INCLUDE_DIR "${NAPI_V8_HB_PREFIX}/include" CACHE PATH "Path to V8 headers" FORCE)
endif()
if(NAPI_V8_V8_EXTRA_LIBS STREQUAL "" AND NAPI_V8_V8_EXTRA_LIBS_DEFAULT)
  set(NAPI_V8_V8_EXTRA_LIBS "${NAPI_V8_V8_EXTRA_LIBS_DEFAULT}" CACHE STRING "Semicolon-separated extra V8 libraries (e.g. libplatform/libbase)" FORCE)
endif()
if(NOT DEFINED NAPI_V8_V8_EXTRA_LIBS_DEFAULT)
  set(NAPI_V8_V8_EXTRA_LIBS_DEFAULT "")
endif()
if(DEFINED ENV{NAPI_V8_V8_EXTRA_LIBS})
  set(NAPI_V8_V8_EXTRA_LIBS_DEFAULT "$ENV{NAPI_V8_V8_EXTRA_LIBS}")
endif()
set(NAPI_V8_V8_EXTRA_LIBS
  "${NAPI_V8_V8_EXTRA_LIBS_DEFAULT}"
  CACHE STRING "Semicolon-separated extra V8 libraries (e.g. libplatform/libbase)"
)

add_library(napi_v8 STATIC
  src/js_native_api_v8.cc
  src/unofficial_napi.cc
)

target_include_directories(napi_v8
  PUBLIC
    "${NAPI_V8_ROOT}/include"
  PRIVATE
    "${NAPI_V8_ROOT}/src"
    "${NAPI_V8_V8_INCLUDE_DIR}"
)

target_compile_definitions(napi_v8
  PUBLIC
    NAPI_EXTERN=
)
if(NAPI_V8_V8_DEFINES)
  target_compile_definitions(napi_v8 PRIVATE ${NAPI_V8_V8_DEFINES})
endif()
if(NAPI_V8_V8_LIBRARY)
  target_link_libraries(napi_v8 PUBLIC "${NAPI_V8_V8_LIBRARY}")
endif()
if(NAPI_V8_V8_EXTRA_LIBS)
  target_link_libraries(napi_v8 PUBLIC ${NAPI_V8_V8_EXTRA_LIBS})
endif()

if(NAPI_V8_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
