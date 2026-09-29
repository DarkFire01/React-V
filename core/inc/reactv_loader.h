/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Loader interface implemented by every ReactV shim
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_LOADER_H_
#define _REACTV_LOADER_H_

#include <ntddk.h>

/* Bumped whenever the table layout or any call contract changes */
#define REACTV_LOADER_IFACE_VERSION 4

/* MEMORY *********************************************************************/

/*
 * Purpose of a physical allocation. The shim maps each tag to an OS memory
 * type. Everything but ReactVMemoryTransition stays reserved permanently.
 */
typedef enum _REACTV_MEMORY_TAG
{
    ReactVMemoryImage = 0,
    ReactVMemoryHeap,
    ReactVMemoryPageTable,
    ReactVMemoryTransition,
    ReactVMemoryTagMaximum
} REACTV_MEMORY_TAG;

typedef enum _REACTV_MEMORY_KIND
{
    ReactVMemoryKindFree = 0,
    ReactVMemoryKindInUse,
    ReactVMemoryKindReserved,
    ReactVMemoryKindBad,
    ReactVMemoryKindMaximum
} REACTV_MEMORY_KIND;

typedef struct _REACTV_MEMORY_RANGE
{
    ULONG64 BasePage;
    ULONG64 PageCount;
    ULONG Kind;
    ULONG Reserved;
} REACTV_MEMORY_RANGE, *PREACTV_MEMORY_RANGE;

typedef struct _REACTV_MEMORY_IFACE
{
    /*
     * Loader virtual range holding the core image, reserved permanently so the
     * core can map it at the same addresses. Only the virtual range is
     * contiguous, translate it per page.
     */
    _Must_inspect_result_
    NTSTATUS
    (NTAPI *ReserveCoreImage)(
        _Out_ ULONG64 *VirtualBase,
        _Out_ ULONG64 *Length);

    _Must_inspect_result_
    NTSTATUS
    (NTAPI *AllocatePages)(
        _In_ ULONG64 PageCount,
        _In_ ULONG64 AlignmentInPages,
        _In_ ULONG Tag,
        _Out_ ULONG64 *PhysicalBase);

    VOID
    (NTAPI *FreePages)(
        _In_ ULONG64 PhysicalBase,
        _In_ ULONG64 PageCount);

    /*
     * AP startup page. The SIPI vector is a page number, so it must be below
     * 1 MB. Reserved permanently.
     */
    _Must_inspect_result_
    NTSTATUS
    (NTAPI *AllocateStartupPage)(
        _Out_ ULONG64 *PhysicalBase);

    _Must_inspect_result_
    NTSTATUS
    (NTAPI *MapPhysical)(
        _In_ ULONG64 PhysicalBase,
        _In_ ULONG64 Length,
        _Outptr_ PVOID *Virtual);

    VOID
    (NTAPI *UnmapVirtual)(
        _In_ PVOID Virtual,
        _In_ ULONG64 Length);

    _Must_inspect_result_
    NTSTATUS
    (NTAPI *TranslateVirtual)(
        _In_ PVOID Virtual,
        _Out_ ULONG64 *PhysicalBase);

    /*
     * On entry *RangeCount is the capacity of Ranges, zero to ask for the
     * count alone. On return it is the number of ranges the firmware map
     * holds, and STATUS_BUFFER_TOO_SMALL says the capacity was short.
     */
    _Must_inspect_result_
    NTSTATUS
    (NTAPI *GetMemoryMap)(
        _Inout_ ULONG64 *RangeCount,
        _Out_writes_opt_(*RangeCount) PREACTV_MEMORY_RANGE Ranges);

    ULONG64
    (NTAPI *GetHighestPhysicalPage)(VOID);
} REACTV_MEMORY_IFACE, *PREACTV_MEMORY_IFACE;

/* SYSTEM DESCRIPTION *********************************************************/

#define REACTV_PROCESSOR_ENABLED        0x00000001
#define REACTV_PROCESSOR_ONLINE_CAPABLE 0x00000002

typedef struct _REACTV_PROCESSOR_INFO
{
    ULONG ApicId;
    ULONG NumaNode;
    ULONG Flags;
    ULONG Reserved;
} REACTV_PROCESSOR_INFO, *PREACTV_PROCESSOR_INFO;

typedef struct _REACTV_SYSTEM_IFACE
{
    /* Signature is the four table characters in little-endian order */
    _Must_inspect_result_
    NTSTATUS
    (NTAPI *FindAcpiTable)(
        _In_ ULONG Signature,
        _Out_ ULONG64 *PhysicalAddress,
        _Out_opt_ ULONG *Length);

    /* Same in/out convention as GetMemoryMap */
    _Must_inspect_result_
    NTSTATUS
    (NTAPI *QueryProcessors)(
        _Inout_ ULONG *ProcessorCount,
        _Out_writes_opt_(*ProcessorCount) PREACTV_PROCESSOR_INFO Processors);

    ULONG64
    (NTAPI *ReadMsr)(
        _In_ ULONG Index);

    /* Number is the control register index, so 0, 2, 3, 4 or 8 */
    ULONG64
    (NTAPI *ReadControlRegister)(
        _In_ ULONG Number);

    VOID
    (NTAPI *Cpuid)(
        _In_ ULONG Leaf,
        _In_ ULONG SubLeaf,
        _Out_writes_(4) ULONG *Registers);
} REACTV_SYSTEM_IFACE, *PREACTV_SYSTEM_IFACE;

/* LAUNCH *********************************************************************/

#include <pshpack1.h>
typedef struct _REACTV_DESCRIPTOR
{
    USHORT Limit;
    ULONG64 Base;
} REACTV_DESCRIPTOR, *PREACTV_DESCRIPTOR;
#include <poppack.h>

typedef struct _REACTV_SEGMENT_STATE
{
    USHORT Cs;
    USHORT Ss;
    USHORT Ds;
    USHORT Es;
    USHORT Fs;
    USHORT Gs;
    USHORT Tr;
    USHORT Ldtr;
} REACTV_SEGMENT_STATE, *PREACTV_SEGMENT_STATE;

/* Root state loaded into the guest at launch. The core sets RIP and RSP. */
typedef struct _REACTV_RESUME_STATE
{
    ULONG64 Rflags;
    ULONG64 Cr0;
    ULONG64 Cr3;
    ULONG64 Cr4;
    ULONG64 Efer;
    ULONG64 Pat;
} REACTV_RESUME_STATE, *PREACTV_RESUME_STATE;

/* XCr0 holds a captured value, cleared when the host has no XSAVE */
#define REACTV_LAUNCH_XSAVE_VALID 0x00000001

typedef struct _REACTV_LAUNCH_CONTEXT
{
    ULONG Size;
    ULONG Flags;
    REACTV_SEGMENT_STATE Segments;
    REACTV_DESCRIPTOR Gdtr;
    REACTV_DESCRIPTOR Idtr;
    ULONG64 FsBase;
    ULONG64 GsBase;
    ULONG64 KernelGsBase;
    ULONG64 XCr0;
    REACTV_RESUME_STATE Resume;
} REACTV_LAUNCH_CONTEXT, *PREACTV_LAUNCH_CONTEXT;

typedef struct _REACTV_LAUNCH_IFACE
{
    /* Captures machine state only the loader knows */
    VOID
    (NTAPI *CaptureContext)(
        _Out_ PREACTV_LAUNCH_CONTEXT Context);

    /* Called with interrupts already off, right before the core takes over */
    VOID
    (NTAPI *Quiesce)(VOID);

    /* Called once the root is running again, launched or not */
    VOID
    (NTAPI *Resumed)(
        _In_ NTSTATUS Status);
} REACTV_LAUNCH_IFACE, *PREACTV_LAUNCH_IFACE;

/* DEBUG AND LOG **************************************************************/

#define REACTV_LOG_ERROR   0
#define REACTV_LOG_WARNING 1
#define REACTV_LOG_TRACE   2

typedef struct _REACTV_DEBUG_IFACE
{
    VOID
    (__cdecl *Log)(
        _In_ ULONG Level,
        _In_z_ _Printf_format_string_ PCSTR Format,
        ...);

    /* Opaque debug device for the core's transport, or NULL */
    PVOID DebugDevice;
} REACTV_DEBUG_IFACE, *PREACTV_DEBUG_IFACE;

/* THE TABLE ******************************************************************/

typedef struct _REACTV_LOADER_IFACE
{
    ULONG Version;
    ULONG Size;
    PREACTV_MEMORY_IFACE Memory;
    PREACTV_SYSTEM_IFACE System;
    PREACTV_LAUNCH_IFACE Launch;
    PREACTV_DEBUG_IFACE Debug;
} REACTV_LOADER_IFACE, *PREACTV_LOADER_IFACE;

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVCoreLaunch(
    _In_ PREACTV_LOADER_IFACE Loader);

#endif /* _REACTV_LOADER_H_ */
