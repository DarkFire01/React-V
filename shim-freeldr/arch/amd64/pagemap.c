/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Identity mapping of physical memory above the boot map
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

#include <ndk/asm.h>
#include <internal/amd64/intrin_i.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(HYPERVISOR);

/*
 * The real mode startup code identity maps the low 1 GB with 2 MB pages using
 * one PML4, PDP and PD at PML4_ADDRESS, PDP_ADDRESS and PD_ADDRESS. On real
 * hardware the ACPI tables are often above that. Extensions stay identity mapped.
 */

#define REACTV_LARGE_PAGE_SIZE (1ULL << PDI_SHIFT)
#define REACTV_GIANT_PAGE_SIZE (1ULL << PPI_SHIFT)

/* FUNCTIONS ******************************************************************/

static
PHARDWARE_PTE
NTAPI
ReactVTableFromEntry(
    _In_ PHARDWARE_PTE Entry)
{
    return (PHARDWARE_PTE)(ULONG_PTR)(Entry->PageFrameNumber << MM_PAGE_SHIFT);
}

static
PHARDWARE_PTE
NTAPI
ReactVCreateTable(
    _Inout_ PHARDWARE_PTE Entry)
{
    PVOID Page;

    /* The new table is written through the current map, so it must be below the boot limit */
    Page = MmAllocateHighestMemoryBelowAddress(MM_PAGE_SIZE,
                                               (PVOID)REACTV_BOOT_IDENTITY_LIMIT,
                                               LoaderFirmwareTemporary);
    if (Page == NULL)
    {
        RV_ERR("No page table page below %I64x\n",
            (ULONG64)REACTV_BOOT_IDENTITY_LIMIT);
        return NULL;
    }

    RtlZeroMemory(Page, MM_PAGE_SIZE);

    Entry->PageFrameNumber = (ULONG_PTR)Page >> MM_PAGE_SHIFT;
    Entry->Write = 1;
    Entry->Valid = 1;

    return Page;
}

#ifdef UEFIBOOT
/**
 * @brief
 * Checks whether a page is identity mapped in the live page tables.
 *
 * @remarks
 * Walks from CR3 since the UEFI tables are not the loader's. A 2 MB or 1 GB
 * page ends the walk.
 */
static
BOOLEAN
NTAPI
ReactVIdentityMapped(
    _In_ ULONG64 Address)
{
    PHARDWARE_PTE Table;
    ULONG64 Next;
    ULONG Level;

    /* PML4, PDPT, PD, PT index shifts */
    const ULONG Shift[4] = { PXI_SHIFT, PPI_SHIFT, PDI_SHIFT, PTI_SHIFT };

    Next = ALIGN_DOWN_BY(__readcr3(), MM_PAGE_SIZE);

    for (Level = 0; Level < RTL_NUMBER_OF(Shift); Level++)
    {
        PHARDWARE_PTE Entry;

        Table = (PHARDWARE_PTE)(ULONG_PTR)Next;
        Entry = &Table[(Address >> Shift[Level]) & 0x1FF];

        if (!Entry->Valid)
            return FALSE;

        /* PS ends the walk. At the PT level bit 7 is PAT and every entry is a page. */
        if ((Level == RTL_NUMBER_OF(Shift) - 1) || Entry->LargePage)
            return TRUE;

        Next = (ULONG64)Entry->PageFrameNumber << MM_PAGE_SHIFT;
    }

    return TRUE;
}
#endif

static
BOOLEAN
NTAPI
ReactVMapLargePage(
    _In_ ULONG64 PhysicalBase,
    _Out_ PBOOLEAN Created)
{
    PHARDWARE_PTE Table, Entry;

    *Created = FALSE;

    /* Only the loader's own tables can be extended */
    if (ALIGN_DOWN_BY(__readcr3(), MM_PAGE_SIZE) != PML4_ADDRESS)
    {
        RV_ERR("CR3 is %I64x, not the loader's PML4\n", (ULONG64)__readcr3());
        return FALSE;
    }

    Table = (PHARDWARE_PTE)(ULONG_PTR)PML4_ADDRESS;
    Entry = &Table[VAtoPXI(PhysicalBase)];
    Table = Entry->Valid ? ReactVTableFromEntry(Entry) : ReactVCreateTable(Entry);
    if (Table == NULL)
        return FALSE;

    Entry = &Table[VAtoPPI(PhysicalBase)];
    if (Entry->Valid && Entry->LargePage)
    {
        /* Already covered by a 1 GB page. freeldr builds none, so only verify it */
        return (Entry->PageFrameNumber ==
                (ALIGN_DOWN_BY(PhysicalBase, REACTV_GIANT_PAGE_SIZE) >> MM_PAGE_SHIFT));
    }

    Table = Entry->Valid ? ReactVTableFromEntry(Entry) : ReactVCreateTable(Entry);
    if (Table == NULL)
        return FALSE;

    Entry = &Table[VAtoPDI(PhysicalBase)];
    if (Entry->Valid)
    {
        /* Only a 2 MB identity mapping of this page counts */
        return (Entry->LargePage &&
                (Entry->PageFrameNumber == (PhysicalBase >> MM_PAGE_SHIFT)));
    }

    Entry->PageFrameNumber = PhysicalBase >> MM_PAGE_SHIFT;
    Entry->LargePage = 1;
    Entry->Write = 1;
    Entry->Valid = 1;

    *Created = TRUE;
    return TRUE;
}

BOOLEAN
NTAPI
ReactVMapIdentityRange(
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length)
{
    ULONG64 Address, End;
#ifndef UEFIBOOT
    BOOLEAN Created, Added = FALSE;
#endif

#ifdef UEFIBOOT
    /* Firmware owns these tables: verify the range is identity mapped, never modify them */
    Address = ALIGN_DOWN_BY(PhysicalBase, MM_PAGE_SIZE);
    End = ALIGN_UP_BY(PhysicalBase + Length, MM_PAGE_SIZE);

    for (; Address < End; Address += MM_PAGE_SIZE)
    {
        if (!ReactVIdentityMapped(Address))
        {
            RV_ERR("%I64x not identity mapped by firmware\n", Address);
            return FALSE;
        }
    }

    return TRUE;
#else
    Address = ALIGN_DOWN_BY(PhysicalBase, REACTV_LARGE_PAGE_SIZE);
    End = ALIGN_UP_BY(PhysicalBase + Length, REACTV_LARGE_PAGE_SIZE);

    for (; Address < End; Address += REACTV_LARGE_PAGE_SIZE)
    {
        if (Address < REACTV_BOOT_IDENTITY_LIMIT)
            continue;

        if (!ReactVMapLargePage(Address, &Created))
        {
            RV_ERR("Cannot map %I64x\n", Address);
            return FALSE;
        }

        if (Created)
            Added = TRUE;
    }

    if (Added)
    {
        /* Not present to present needs no flush, but the tables were written through this map */
        __writecr3(__readcr3());
        RV_TRACE("Mapped %I64x for %I64x bytes\n", PhysicalBase, Length);
    }

    return TRUE;
#endif
}
