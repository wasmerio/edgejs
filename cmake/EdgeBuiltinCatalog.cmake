function(edge_add_builtin_catalog target_name)
  if(NOT TARGET edge_builtin_catalog_gen)
    file(GLOB_RECURSE EDGE_BUILTIN_LIB_SOURCES CONFIGURE_DEPENDS
      "${PROJECT_ROOT}/lib/*.js"
    )
    file(GLOB_RECURSE EDGE_BUILTIN_DEP_SOURCES CONFIGURE_DEPENDS
      "${PROJECT_ROOT}/deps/*.js"
      "${PROJECT_ROOT}/deps/*.mjs"
    )

    set(EDGE_BUILTIN_CATALOG_HEADER
      "${CMAKE_CURRENT_BINARY_DIR}/edge_builtin_catalog_data.h"
    )
    # Built as a configure-time list rather than a generator expression: under
    # VERBATIM an empty genex still becomes an empty '' argument, which the
    # generator rejects. An empty list expands to no argument at all.
    set(EDGE_BUILTIN_CATALOG_ARGS "")
    if(NOT EDGE_EMBED_BUILTIN_SOURCES)
      list(APPEND EDGE_BUILTIN_CATALOG_ARGS --no-sources)
    endif()
    # The Makefile generator does not rerun a custom command when only its
    # arguments change, so flipping EDGE_EMBED_BUILTIN_SOURCES in an existing
    # build dir would keep a stale header. This stamp is rewritten only when
    # the mode changes, and the header depends on it.
    set(EDGE_BUILTIN_CATALOG_MODE_STAMP
      "${CMAKE_CURRENT_BINARY_DIR}/edge_builtin_catalog_mode.txt"
    )
    file(CONFIGURE
      OUTPUT "${EDGE_BUILTIN_CATALOG_MODE_STAMP}"
      CONTENT "EDGE_EMBED_BUILTIN_SOURCES=${EDGE_EMBED_BUILTIN_SOURCES}\n"
    )
    add_custom_command(
      OUTPUT "${EDGE_BUILTIN_CATALOG_HEADER}"
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_ROOT}/scripts/generate_builtin_catalog_header.py"
        --project-root "${PROJECT_ROOT}"
        --output "${EDGE_BUILTIN_CATALOG_HEADER}"
        ${EDGE_BUILTIN_CATALOG_ARGS}
      DEPENDS
        "${PROJECT_ROOT}/scripts/generate_builtin_catalog_header.py"
        "${EDGE_BUILTIN_CATALOG_MODE_STAMP}"
        ${EDGE_BUILTIN_LIB_SOURCES}
        ${EDGE_BUILTIN_DEP_SOURCES}
      COMMENT "Generating Edge builtin catalog"
      VERBATIM
    )
    add_custom_target(edge_builtin_catalog_gen DEPENDS "${EDGE_BUILTIN_CATALOG_HEADER}")
  endif()
  add_dependencies("${target_name}" edge_builtin_catalog_gen)
  target_include_directories("${target_name}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
