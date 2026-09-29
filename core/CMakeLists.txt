##
## PROJECT:     ReactV
## LICENSE:     MIT (https://spdx.org/licenses/MIT)
## PURPOSE:     Hypervisor core, one library per shim
## COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
##

list(APPEND REACTV_CORE_SOURCE
    apic.c
    child.c
    core.c
    detect.c
    ap.c
    exit.c
    guest.c
    hv.c
    paging.c
    partition.c
    ports.c
    serial.c
    svm.c
    synic.c
    vmx.c
    vmxexit.c)

foreach(_host freeldr winload)
    set(_target reactv_core_${_host})
    set(_asm_list reactv_core_${_host}_asm)

    add_asm_files(${_asm_list} arch/amd64/svm.S arch/amd64/vmx.S)

    add_library(${_target} ${REACTV_CORE_SOURCE} ${${_asm_list}})
    target_include_directories(${_target} PUBLIC inc)
    target_compile_definitions(${_target} PRIVATE _NTHAL_ _BLDR_ _NTSYSTEM_)
    string(TOUPPER ${_host} _host_upper)
    target_compile_definitions(${_target} PRIVATE REACTV_HOST_${_host_upper})
    add_dependencies(${_target} asm xdk)

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # SSE is not enabled in the loader environment
        target_compile_options(${_target} PUBLIC -mno-sse)
    endif()
endforeach()
