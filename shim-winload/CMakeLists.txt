##
## PROJECT:     ReactV
## LICENSE:     MIT (https://spdx.org/licenses/MIT)
## PURPOSE:     ReactV loader interface for the Windows boot library
## COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
##

add_library(reactv_shim_winload
    launch.c
    memory.c
    shim.c
    system.c)

target_compile_definitions(reactv_shim_winload PRIVATE _NTHAL_ _BLDR_ _NTSYSTEM_)
target_link_libraries(reactv_shim_winload reactv_core_winload)
add_dependencies(reactv_shim_winload xdk)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(reactv_shim_winload PUBLIC -mno-sse)
endif()
