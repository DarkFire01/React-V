##
## PROJECT:     ReactV
## LICENSE:     MIT (https://spdx.org/licenses/MIT)
## PURPOSE:     ReactV loader interface for freeldr
## COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
##

# freeldr headers (debug.h in particular) must shadow the SDK ones
include_directories(BEFORE
    ${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include
    ${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr
    ${REACTOS_SOURCE_DIR}/ntoskrnl/include)

list(APPEND REACTV_SHIM_SOURCE
    arch/amd64/pagemap.c
    launch.c
    memory.c
    probe.c
    shim.c
    system.c)

# One library per firmware flavor. PC/AT scans low memory for the RSDP, UEFI gets it from the system table
foreach(_firmware pcat uefi)
    set(_target reactv_shim_freeldr)
    if(_firmware STREQUAL "uefi")
        set(_target reactv_shim_uefildr)
    endif()

    set(_asm_list ${_target}_asm)
    add_asm_files(${_asm_list} arch/amd64/capture.S)

    add_library(${_target} ${REACTV_SHIM_SOURCE} ${${_asm_list}})

    target_include_directories(${_target} PUBLIC inc)
    target_compile_definitions(${_target} PRIVATE _NTHAL_ _BLDR_ _NTSYSTEM_)
    target_link_libraries(${_target} reactv_core_freeldr)
    add_dependencies(${_target} asm bugcodes xdk)

    if(_firmware STREQUAL "uefi")
        target_compile_definitions(${_target} PRIVATE UEFIBOOT)
        # EFI headers, same paths uefildr uses
        target_include_directories(${_target} BEFORE PRIVATE
            ${REACTOS_SOURCE_DIR}/sdk/include/reactos/edk2
            ${REACTOS_SOURCE_DIR}/boot/freeldr/freeldr/include/arch/uefi)
    endif()

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${_target} PUBLIC -mno-sse)
    endif()
endforeach()
