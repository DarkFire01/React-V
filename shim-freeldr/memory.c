/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Loader memory interface over the freeldr memory manager
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

#ifdef UEFIBOOT
/* Low stub the UEFI loader reserved below 1 MB, and its size in pages */
extern ULONG_PTR UefiLowStubBase;
#define REACTV_LOW_STUB_PAGES 8
#endif

#include <debug.h>
DBG_DEFAULT_CHANNEL(HYPERVISOR);

/* GLOBALS ********************************************************************/

/* Over-aligned allocations keep their slack, so track the freeldr base for freeing */
#define REACTV_TRACKED_ALLOCATIONS 32

typedef struct _REACTV_ALLOCATION
{
    ULONG64 AlignedBase;
    PVOID Base;
} REACTV_ALLOCATION, *PREACTV_ALLOCATION;

static REACTV_ALLOCATION ReactVAllocations[REACTV_TRACKED_ALLOCATIONS];

/* FUNCTIONS ******************************************************************/

static
TYPE_OF_MEMORY
NTAPI
ReactVMemoryTypeFromTag(
    _In_ ULONG Tag)
{
    /* Core memory must survive the kernel. Transition pages are reclaimable after launch. */
    if (Tag == ReactVMemoryTransition)
        return LoaderFirmwareTemporary;

    return LoaderSpecialMemory;
}

static
ULONG
NTAPI
ReactVMemoryKindFromType(
    _In_ TYPE_OF_MEMORY Type)
{
    switch (Type)
    {
        case LoaderFree:
            return ReactVMemoryKindFree;

        case LoaderBad:
            return ReactVMemoryKindBad;

        case LoaderFirmwarePermanent:
        case LoaderSpecialMemory:
        case LoaderHALCachedMemory:
        case LoaderBBTMemory:
            return ReactVMemoryKindReserved;

        default:
            return ReactVMemoryKindInUse;
    }
}

static
VOID
NTAPI
ReactVTrackAllocation(
    _In_ ULONG64 AlignedBase,
    _In_ PVOID Base)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(ReactVAllocations); Index++)
    {
        if (ReactVAllocations[Index].Base == NULL)
        {
            ReactVAllocations[Index].AlignedBase = AlignedBase;
            ReactVAllocations[Index].Base = Base;
            return;
        }
    }

    RV_WARN("Allocation slots full, %I64x not tracked\n", AlignedBase);
}

static
PVOID
NTAPI
ReactVUntrackAllocation(
    _In_ ULONG64 AlignedBase)
{
    ULONG Index;
    PVOID Base;

    for (Index = 0; Index < RTL_NUMBER_OF(ReactVAllocations); Index++)
    {
        if ((ReactVAllocations[Index].Base != NULL) &&
            (ReactVAllocations[Index].AlignedBase == AlignedBase))
        {
            Base = ReactVAllocations[Index].Base;
            ReactVAllocations[Index].AlignedBase = 0;
            ReactVAllocations[Index].Base = NULL;
            return Base;
        }
    }

    return (PVOID)(ULONG_PTR)AlignedBase;
}

static
NTSTATUS
NTAPI
ReactVReserveCoreImage(
    _Out_ ULONG64 *VirtualBase,
    _Out_ ULONG64 *Length)
{
    PIMAGE_NT_HEADERS NtHeaders;
    ULONG64 Base, Size;

    /*
     * Reserve the whole loader image: MSVC places pooled literals, unwind data
     * and CRT helpers outside any section the core can name. It is about 256 KB.
     */
    NtHeaders = RtlImageNtHeader(&__ImageBase);
    if (NtHeaders == NULL)
    {
        RV_ERR("Loader image has no NT headers\n");
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    Base = (ULONG64)(ULONG_PTR)&__ImageBase;
    Size = ALIGN_UP_BY(NtHeaders->OptionalHeader.SizeOfImage, MM_PAGE_SIZE);

    /*
     * LoaderFirmwarePermanent, not LoaderSpecialMemory: freeldr leaves special
     * memory out of the kernel page tables, and the loader runs from this image
     * until it switches to them.
     */
    MmSetMemoryType((PVOID)(ULONG_PTR)Base, (SIZE_T)Size, LoaderFirmwarePermanent);

    *VirtualBase = Base;
    *Length = Size;

    RV_TRACE("Reserved loader image %I64x, %I64x bytes\n", Base, Size);
    return STATUS_SUCCESS;
}

/* SIPI vector is an 8 bit page number. Page 0 holds the IVT and BDA. */
#define REACTV_STARTUP_PAGE_LIMIT 0x100000
#define REACTV_STARTUP_PAGE_FIRST 0x1000

static
NTSTATUS
NTAPI
ReactVAllocateStartupPage(
    _Out_ ULONG64 *PhysicalBase)
{
#ifdef UEFIBOOT
    ULONG64 Page;

    /*
     * Boot services are exited, so use the last page of the loader's low stub.
     * The kernel uses the start of it for AP startup.
     */
    if (UefiLowStubBase == 0)
    {
        RV_ERR("No low stub below 1 MB\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Page = (ULONG64)UefiLowStubBase +
           ((REACTV_LOW_STUB_PAGES - 1) * MM_PAGE_SIZE);

    if (!ReactVMapIdentityRange(Page, MM_PAGE_SIZE))
        return STATUS_NOT_MAPPED_VIEW;

    RV_TRACE("SIPI page at %I64x\n", Page);

    *PhysicalBase = Page;
    return STATUS_SUCCESS;
#else
    ULONG64 Candidate;

    /*
     * Probe page by page from the top down. MmAllocateHighestMemoryBelowAddress
     * is avoided because on failure it waits for a key, which hangs a headless boot.
     */
    for (Candidate = REACTV_STARTUP_PAGE_LIMIT - MM_PAGE_SIZE;
         Candidate >= REACTV_STARTUP_PAGE_FIRST;
         Candidate -= MM_PAGE_SIZE)
    {
        PVOID Base = MmAllocateMemoryAtAddress(MM_PAGE_SIZE,
                                               (PVOID)(ULONG_PTR)Candidate,
                                               ReactVMemoryTypeFromTag(ReactVMemoryImage));
        if (Base == NULL)
            continue;

        if (!ReactVMapIdentityRange(Candidate, MM_PAGE_SIZE))
        {
            MmFreeMemory(Base);
            return STATUS_NOT_MAPPED_VIEW;
        }

        RV_TRACE("SIPI page at %I64x\n", Candidate);

        *PhysicalBase = Candidate;
        return STATUS_SUCCESS;
    }

    RV_ERR("No free SIPI page below 1 MB\n");
    return STATUS_INSUFFICIENT_RESOURCES;
#endif
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
    PVOID Base;
    ULONG64 AlignedBase;
    SIZE_T Size;

    if ((PageCount == 0) || (Tag >= ReactVMemoryTagMaximum))
        return STATUS_INVALID_PARAMETER;

    if (AlignmentInPages == 0)
        AlignmentInPages = 1;

    /* freeldr only page aligns, so over-allocate and align up */
    Size = (SIZE_T)((PageCount + AlignmentInPages - 1) * MM_PAGE_SIZE);

    Base = MmAllocateMemoryWithType(Size, ReactVMemoryTypeFromTag(Tag));
    if (Base == NULL)
    {
        RV_ERR("Cannot allocate %I64u pages for tag %lu\n", PageCount, Tag);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    AlignedBase = ALIGN_UP_BY((ULONG64)(ULONG_PTR)Base,
                              AlignmentInPages * MM_PAGE_SIZE);

    /* freeldr can return pages outside its identity map */
    if (!ReactVMapIdentityRange(AlignedBase, PageCount * MM_PAGE_SIZE))
    {
        MmFreeMemory(Base);
        return STATUS_NOT_MAPPED_VIEW;
    }

    if (AlignedBase != (ULONG64)(ULONG_PTR)Base)
        ReactVTrackAllocation(AlignedBase, Base);

    *PhysicalBase = AlignedBase;
    RV_TRACE("Allocated %I64u pages at %I64x for tag %lu\n",
          PageCount, AlignedBase, Tag);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
ReactVFreePages(
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 PageCount)
{
    UNREFERENCED_PARAMETER(PageCount);

    MmFreeMemory(ReactVUntrackAllocation(PhysicalBase));
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVShimMapPhysical(
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length,
    _Outptr_ PVOID *Virtual)
{
    if ((Length == 0) || (PhysicalBase + Length > REACTV_PHYSICAL_MAX))
    {
        RV_ERR("Cannot map %I64x, %I64x bytes\n", PhysicalBase, Length);
        return STATUS_INVALID_PARAMETER;
    }

    if (!ReactVMapIdentityRange(PhysicalBase, Length))
        return STATUS_NOT_MAPPED_VIEW;

    /* Identity map, VA equals PA */
    *Virtual = (PVOID)(ULONG_PTR)PhysicalBase;
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
ReactVUnmapVirtual(
    _In_ PVOID Virtual,
    _In_ ULONG64 Length)
{
    /* Nothing to tear down, the identity map dies with the loader page tables */
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
    if ((ULONG64)(ULONG_PTR)Virtual >= REACTV_PHYSICAL_MAX)
    {
        RV_ERR("%p is outside the identity map\n", Virtual);
        return STATUS_NOT_MAPPED_VIEW;
    }

    *PhysicalBase = (ULONG64)(ULONG_PTR)Virtual;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
ReactVGetMemoryMap(
    _Inout_ ULONG64 *RangeCount,
    _Out_writes_opt_(*RangeCount) PREACTV_MEMORY_RANGE Ranges)
{
    PFREELDR_MEMORY_DESCRIPTOR Descriptors;
    ULONG64 Capacity;
    ULONG Count, Index;

    Count = MmGetBiosMemoryMap(&Descriptors);
    Capacity = *RangeCount;
    *RangeCount = Count;

    if ((Ranges == NULL) || (Capacity < Count))
        return STATUS_BUFFER_TOO_SMALL;

    for (Index = 0; Index < Count; Index++)
    {
        Ranges[Index].BasePage = Descriptors[Index].BasePage;
        Ranges[Index].PageCount = Descriptors[Index].PageCount;
        Ranges[Index].Kind = ReactVMemoryKindFromType(Descriptors[Index].MemoryType);
        Ranges[Index].Reserved = 0;
    }

    return STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVGetHighestPhysicalPage(VOID)
{
    return MmGetHighestPhysicalPage();
}

REACTV_MEMORY_IFACE ReactVShimMemory =
{
    ReactVReserveCoreImage,
    ReactVAllocatePages,
    ReactVFreePages,
    ReactVAllocateStartupPage,
    ReactVShimMapPhysical,
    ReactVUnmapVirtual,
    ReactVTranslateVirtual,
    ReactVGetMemoryMap,
    ReactVGetHighestPhysicalPage
};
