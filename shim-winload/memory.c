/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     winload memory interface (stubs)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

/* FUNCTIONS ******************************************************************/

static
NTSTATUS
NTAPI
ReactVReserveCoreImage(
    _Out_ ULONG64 *VirtualBase,
    _Out_ ULONG64 *Length)
{
    /* Find the core image and keep its pages reserved past the loader */
    UNREFERENCED_PARAMETER(VirtualBase);
    UNREFERENCED_PARAMETER(Length);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
ReactVAllocatePages(
    _In_ ULONG64 PageCount,
    _In_ ULONG64 AlignmentInPages,
    _In_ ULONG Tag,
    _Out_ ULONG64 *PhysicalBase)
{
    /* Allocate physical pages, persisting the ones that outlive the loader */
    UNREFERENCED_PARAMETER(PageCount);
    UNREFERENCED_PARAMETER(AlignmentInPages);
    UNREFERENCED_PARAMETER(Tag);
    UNREFERENCED_PARAMETER(PhysicalBase);

    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
ReactVFreePages(
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 PageCount)
{
    /* Free physical pages */
    UNREFERENCED_PARAMETER(PhysicalBase);
    UNREFERENCED_PARAMETER(PageCount);
}

static
NTSTATUS
NTAPI
ReactVAllocateStartupPage(
    _Out_ ULONG64 *PhysicalBase)
{
    /* Allocate physical pages below 1MB */
    UNREFERENCED_PARAMETER(PhysicalBase);
    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
ReactVMapPhysical(
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length,
    _Outptr_ PVOID *Virtual)
{
    /* Map a physical range */
    UNREFERENCED_PARAMETER(PhysicalBase);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Virtual);

    return STATUS_NOT_IMPLEMENTED;
}

static
VOID
NTAPI
ReactVUnmapVirtual(
    _In_ PVOID Virtual,
    _In_ ULONG64 Length)
{
    /* Unmap a virtual range */
    UNREFERENCED_PARAMETER(Virtual);
    UNREFERENCED_PARAMETER(Length);
}

static
NTSTATUS
NTAPI
ReactVTranslateVirtual(
    _In_ PVOID Virtual,
    _Out_ ULONG64 *PhysicalBase)
{
    /* Translate a virtual address */
    UNREFERENCED_PARAMETER(Virtual);
    UNREFERENCED_PARAMETER(PhysicalBase);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
ReactVGetMemoryMap(
    _Inout_ ULONG64 *RangeCount,
    _Out_writes_opt_(*RangeCount) PREACTV_MEMORY_RANGE Ranges)
{
    /* Read the firmware memory map */
    UNREFERENCED_PARAMETER(Ranges);

    *RangeCount = 0;
    return STATUS_NOT_IMPLEMENTED;
}

static
ULONG64
NTAPI
ReactVGetHighestPhysicalPage(VOID)
{
    /* End of the last usable range in the boot library memory map */
    return 0;
}

REACTV_MEMORY_IFACE ReactVShimMemory =
{
    ReactVReserveCoreImage,
    ReactVAllocatePages,
    ReactVFreePages,
    ReactVAllocateStartupPage,
    ReactVMapPhysical,
    ReactVUnmapVirtual,
    ReactVTranslateVirtual,
    ReactVGetMemoryMap,
    ReactVGetHighestPhysicalPage
};
