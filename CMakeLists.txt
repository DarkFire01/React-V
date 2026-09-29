##
## PROJECT:     ReactV
## LICENSE:     MIT (https://spdx.org/licenses/MIT)
## PURPOSE:     ReactV build entry, included from the ReactOS boot tree
## COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
##

# amd64 only
if(NOT ARCH STREQUAL "amd64")
    return()
endif()

set(REACTV_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "ReactV source root")

add_subdirectory(core)
add_subdirectory(shim-freeldr)
add_subdirectory(shim-winload)
