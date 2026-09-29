/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     freeldr shim internal header
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_SHIM_H_
#define _REACTV_SHIM_H_

#include <freeldr.h>

#include "reactv_loader.h"
#include "reactv_svm.h"
#include "reactv_freeldr.h"

/* amd64 boot identity map limit; page tables must be allocated below it */
#define REACTV_BOOT_IDENTITY_LIMIT (((ULONG64)MM_MAX_PAGE_LOADER + 1) << MM_PAGE_SHIFT)

/* Highest mappable address, PFNs are 28 bits */
#define REACTV_PHYSICAL_MAX (1ULL << (28 + MM_PAGE_SHIFT))

/* Shim logging, routed to the core serial port once it is up. Separate from the freeldr debug macros */
VOID
__cdecl
ReactVShimLog(
    _In_ ULONG Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

/* core/serial.c */
BOOLEAN
NTAPI
ReactVSerialReady(VOID);

VOID
NTAPI
ReactVSerialPut(
    _In_z_ PCSTR Text);

#define RV_TRACE(...) ReactVShimLog(REACTV_LOG_TRACE, __VA_ARGS__)
#define RV_WARN(...)  ReactVShimLog(REACTV_LOG_WARNING, __VA_ARGS__)
#define RV_ERR(...)   ReactVShimLog(REACTV_LOG_ERROR, __VA_ARGS__)

VOID
NTAPI
ReactVSerialInitialize(VOID);

extern REACTV_MEMORY_IFACE ReactVShimMemory;
extern REACTV_SYSTEM_IFACE ReactVShimSystem;
extern REACTV_LAUNCH_IFACE ReactVShimLaunch;
extern REACTV_DEBUG_IFACE ReactVShimDebug;

/* memory.c */

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVShimMapPhysical(
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length,
    _Outptr_ PVOID *Virtual);

/* arch/amd64/pagemap.c */

/* Extend the loader identity map to cover the range */
BOOLEAN
NTAPI
ReactVMapIdentityRange(
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length);

/* probe.c */

/* Post-launch hypervisor CPUID probe */
VOID
NTAPI
ReactVProbe(VOID);

/* arch/amd64/capture.S */

/* Read CS, SS, DS, ES, FS, GS, TR, LDTR in that order */
VOID
NTAPI
ReactVFldrReadSelectors(
    _Out_writes_(8) PUSHORT Selectors);

#endif /* _REACTV_SHIM_H_ */
