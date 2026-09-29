/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Core host address space
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"

/*
 * The core image keeps its link time VAs on the same physical pages. All RAM is
 * identity mapped, so page tables, core allocations and any GPA can be used as
 * pointers. MMIO holes are left unmapped so devices can be mapped uncached.
 * Before launch, table writes go through transient loader mappings.
 */

/* Covers the tables for roughly 100 GB of RAM */
#define REACTV_TABLE_POOL_PAGES 128

/*
 * Software available PTE bit marking an identity window mapping. Only these may
 * be overwritten; any other conflicting mapping is an error.
 */
#define REACTV_PTE_WINDOW 0x0000000000000200ULL

/* BIOS maps have a few ranges; UEFI maps can exceed a hundred and grow with CPU count */
#define REACTV_MEMORY_RANGES 512

static REACTV_MEMORY_RANGE ReactVMemoryRanges[REACTV_MEMORY_RANGES];

/* FUNCTIONS ******************************************************************/

static
NTSTATUS
NTAPI
ReactVTableAt(
    _In_ ULONG64 PhysicalBase,
    _Outptr_ PULONG64 *Table)
{
    /* After launch the core's identity map is active; before it, use the loader */
    if (ReactVCore.Live)
    {
        *Table = (PULONG64)(ULONG_PTR)PhysicalBase;
        return STATUS_SUCCESS;
    }

    return ReactVCore.Loader->Memory->MapPhysical(PhysicalBase,
                                                  AMD64_PAGE_SIZE,
                                                  (PVOID *)Table);
}

static
NTSTATUS
NTAPI
ReactVAllocateTablePage(
    _Out_ ULONG64 *PhysicalBase)
{
    PREACTV_ADDRESS_SPACE Space = &ReactVCore.Space;
    PULONG64 Table;
    ULONG64 Page;
    NTSTATUS Status;

    if (Space->TablePoolUsed >= Space->TablePoolPages)
    {
        ReactVLogError("Table pool exhausted, %I64u pages\n",
                       Space->TablePoolPages);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Page = Space->TablePoolBase + (Space->TablePoolUsed * AMD64_PAGE_SIZE);

    Status = ReactVTableAt(Page, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Table, AMD64_PAGE_SIZE);
    Space->TablePoolUsed++;

    *PhysicalBase = Page;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Allocates a permanent core page from the table pool.
 *
 * @remarks
 * Pages are never freed; use only for allocations that live as long as the core.
 */
NTSTATUS
NTAPI
ReactVTakeCorePage(
    _Out_ ULONG64 *PhysicalBase)
{
    return ReactVAllocateTablePage(PhysicalBase);
}

/* Splits a 2 MB PDE into an equivalent page table, e.g. for core image pages inside a large page */
static
NTSTATUS
NTAPI
ReactVSplitLargePage(
    _In_ ULONG64 TableBase,
    _In_ ULONG Index)
{
    PULONG64 Table;
    ULONG64 Large, Base, Flags, Next;
    ULONG Entry;
    NTSTATUS Status;

    Status = ReactVTableAt(TableBase, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    Large = Table[Index];
    Base = Large & AMD64_PTE_ADDRESS_MASK;
    Flags = Large & ~(AMD64_PTE_ADDRESS_MASK | AMD64_PTE_LARGE_PAGE);

    Status = ReactVAllocateTablePage(&Next);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVTableAt(Next, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Small PTEs inherit the large page flags, including REACTV_PTE_WINDOW */
    for (Entry = 0; Entry < (AMD64_PAGE_SIZE / sizeof(ULONG64)); Entry++)
        Table[Entry] = (Base + (Entry * AMD64_PAGE_SIZE)) | Flags;

    Status = ReactVTableAt(TableBase, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    Table[Index] = Next | AMD64_PTE_PRESENT | AMD64_PTE_WRITE;

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
ReactVNextTable(
    _In_ ULONG64 TableBase,
    _In_ ULONG Index,
    _Out_ ULONG64 *NextBase)
{
    PULONG64 Table;
    ULONG64 Next;
    NTSTATUS Status;

    Status = ReactVTableAt(TableBase, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Table[Index] & AMD64_PTE_PRESENT)
    {
        if (Table[Index] & AMD64_PTE_LARGE_PAGE)
        {
            Status = ReactVSplitLargePage(TableBase, Index);
            if (!NT_SUCCESS(Status))
                return Status;

            Status = ReactVTableAt(TableBase, &Table);
            if (!NT_SUCCESS(Status))
                return Status;
        }

        *NextBase = Table[Index] & AMD64_PTE_ADDRESS_MASK;
        return STATUS_SUCCESS;
    }

    Status = ReactVAllocateTablePage(&Next);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Allocation may have reused the loader mapping, so remap the table */
    Status = ReactVTableAt(TableBase, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    Table[Index] = Next | AMD64_PTE_PRESENT | AMD64_PTE_WRITE;

    *NextBase = Next;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
ReactVMapCorePage(
    _In_ ULONG64 VirtualBase,
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Flags)
{
    PULONG64 Table;
    ULONG64 TableBase;
    ULONG Index;
    NTSTATUS Status;

    Status = ReactVNextTable(ReactVCore.Space.Pml4, AMD64_PXI(VirtualBase), &TableBase);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVNextTable(TableBase, AMD64_PPI(VirtualBase), &TableBase);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVNextTable(TableBase, AMD64_PDI(VirtualBase), &TableBase);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVTableAt(TableBase, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    Index = AMD64_PTI(VirtualBase);
    if (Table[Index] & AMD64_PTE_PRESENT)
    {
        if ((Table[Index] & AMD64_PTE_ADDRESS_MASK) == PhysicalBase)
            return STATUS_SUCCESS;

        /* Identity window entries may be replaced, anything else is a conflict */
        if (!(Table[Index] & REACTV_PTE_WINDOW))
        {
            ReactVLogError("VA %I64x already mapped to %I64x, wanted %I64x\n",
                           VirtualBase,
                           Table[Index] & AMD64_PTE_ADDRESS_MASK,
                           PhysicalBase);
            return STATUS_CONFLICTING_ADDRESSES;
        }
    }

    Table[Index] = PhysicalBase | AMD64_PTE_PRESENT | AMD64_PTE_WRITE | Flags;

    /* Flush the stale TLB entry once the core map is live */
    if (ReactVCore.Live)
        __invlpg((PVOID)(ULONG_PTR)VirtualBase);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
ReactVMapCoreRangeFlags(
    _In_ ULONG64 VirtualBase,
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length,
    _In_ ULONG64 Flags)
{
    ULONG64 Offset;
    NTSTATUS Status;

    if ((VirtualBase & AMD64_PAGE_MASK) || (PhysicalBase & AMD64_PAGE_MASK))
        return STATUS_INVALID_PARAMETER;

    for (Offset = 0; Offset < Length; Offset += AMD64_PAGE_SIZE)
    {
        Status = ReactVMapCorePage(VirtualBase + Offset, PhysicalBase + Offset, Flags);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVMapCoreRange(
    _In_ ULONG64 VirtualBase,
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length)
{
    return ReactVMapCoreRangeFlags(VirtualBase, PhysicalBase, Length, 0);
}

/**
 * @brief
 * Maps an MMIO range uncached (PCD set).
 */
_Must_inspect_result_
NTSTATUS
NTAPI
ReactVMapCoreDevice(
    _In_ ULONG64 VirtualBase,
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length)
{
    return ReactVMapCoreRangeFlags(VirtualBase,
                                   PhysicalBase,
                                   Length,
                                   AMD64_PTE_CACHE_DISABLE);
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVAllocateMapped(
    _In_ ULONG64 PageCount,
    _In_ ULONG64 AlignmentInPages,
    _In_ ULONG Tag,
    _Out_ ULONG64 *PhysicalBase)
{
    PREACTV_MEMORY_IFACE Memory = ReactVCore.Loader->Memory;
    ULONG64 Base, Length;
    PVOID Virtual;
    NTSTATUS Status;

    Status = Memory->AllocatePages(PageCount, AlignmentInPages, Tag, &Base);
    if (!NT_SUCCESS(Status))
        return Status;

    Length = PageCount * AMD64_PAGE_SIZE;

    /* RAM is already identity mapped in the core map; zero it through the loader */
    Status = Memory->MapPhysical(Base, Length, &Virtual);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Virtual, (SIZE_T)Length);

    *PhysicalBase = Base;
    return STATUS_SUCCESS;
}

/* Identity maps one 2 MB page; keeps table cost to one page directory per GB */
static
NTSTATUS
NTAPI
ReactVMapCoreLargePage(
    _In_ ULONG64 Address)
{
    PULONG64 Table;
    ULONG64 TableBase;
    ULONG Index;
    NTSTATUS Status;

    Status = ReactVNextTable(ReactVCore.Space.Pml4, AMD64_PXI(Address), &TableBase);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVNextTable(TableBase, AMD64_PPI(Address), &TableBase);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVTableAt(TableBase, &Table);
    if (!NT_SUCCESS(Status))
        return Status;

    Index = AMD64_PDI(Address);
    if (Table[Index] & AMD64_PTE_PRESENT)
    {
        if ((Table[Index] & AMD64_PTE_LARGE_PAGE) &&
            ((Table[Index] & AMD64_PTE_ADDRESS_MASK) == Address))
        {
            return STATUS_SUCCESS;
        }

        ReactVLogError("Cannot map 2 MB page at %I64x, PDE %I64x\n",
                       Address,
                       Table[Index]);
        return STATUS_CONFLICTING_ADDRESSES;
    }

    Table[Index] = Address | AMD64_PTE_PRESENT | AMD64_PTE_WRITE |
                   AMD64_PTE_LARGE_PAGE | REACTV_PTE_WINDOW;
    return STATUS_SUCCESS;
}

/* Identity maps a RAM range, using 2 MB pages where aligned */
static
NTSTATUS
NTAPI
ReactVMapMemoryRange(
    _In_ ULONG64 Start,
    _In_ ULONG64 End)
{
    ULONG64 LargeStart, LargeEnd, Address;
    NTSTATUS Status;

    LargeStart = (Start + AMD64_LARGE_PAGE_MASK) & ~AMD64_LARGE_PAGE_MASK;
    LargeEnd = End & ~AMD64_LARGE_PAGE_MASK;

    /* No aligned 2 MB page inside, map with 4 KB pages */
    if (LargeStart >= LargeEnd)
    {
        return ReactVMapCoreRangeFlags(Start, Start, End - Start,
                                       REACTV_PTE_WINDOW);
    }

    Status = ReactVMapCoreRangeFlags(Start, Start, LargeStart - Start,
                                     REACTV_PTE_WINDOW);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Address = LargeStart; Address < LargeEnd; Address += AMD64_LARGE_PAGE_SIZE)
    {
        Status = ReactVMapCoreLargePage(Address);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return ReactVMapCoreRangeFlags(LargeEnd, LargeEnd, End - LargeEnd,
                                   REACTV_PTE_WINDOW);
}

/**
 * @brief
 * Identity maps all RAM from the firmware memory map.
 *
 * @remarks
 * Holes between ranges stay unmapped so MMIO there can later be mapped uncached.
 */
static
NTSTATUS
NTAPI
ReactVMapAllMemory(VOID)
{
    PREACTV_MEMORY_IFACE Memory = ReactVCore.Loader->Memory;
    ULONG64 Count = REACTV_MEMORY_RANGES;
    ULONG64 Index, Start, End, Mapped = 0;
    NTSTATUS Status;

    Status = Memory->GetMemoryMap(&Count, ReactVMemoryRanges);
    if (!NT_SUCCESS(Status))
    {
        ReactVLogError("Memory map query failed, %lx\n", Status);
        return Status;
    }

    for (Index = 0; Index < Count; Index++)
    {
        /* Skip bad memory */
        if (ReactVMemoryRanges[Index].Kind == ReactVMemoryKindBad)
            continue;

        if (ReactVMemoryRanges[Index].PageCount == 0)
            continue;

        Start = ReactVMemoryRanges[Index].BasePage << AMD64_PAGE_SHIFT;
        End = Start + (ReactVMemoryRanges[Index].PageCount << AMD64_PAGE_SHIFT);

        Status = ReactVMapMemoryRange(Start, End);
        if (!NT_SUCCESS(Status))
            return Status;

        Mapped += ReactVMemoryRanges[Index].PageCount;

        /* Exclusive upper bound for valid GPAs */
        if (End > ReactVCore.MemoryLimit)
            ReactVCore.MemoryLimit = End;
    }

    ReactVLogTrace("Mapped %I64u pages in %I64u ranges\n", Mapped, Count);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
ReactVMapCoreImage(VOID)
{
    PREACTV_MEMORY_IFACE Memory = ReactVCore.Loader->Memory;
    ULONG64 Offset, Physical;
    NTSTATUS Status;

    Status = Memory->ReserveCoreImage(&ReactVCore.ImageBase, &ReactVCore.ImageLength);
    if (!NT_SUCCESS(Status))
    {
        ReactVLogError("Core image reservation failed, %lx\n", Status);
        return Status;
    }

    if ((ReactVCore.ImageBase & AMD64_PAGE_MASK) ||
        (ReactVCore.ImageLength & AMD64_PAGE_MASK) ||
        (ReactVCore.ImageLength == 0))
    {
        ReactVLogError("Core image %I64x, %I64x bytes, not page aligned\n",
                       ReactVCore.ImageBase,
                       ReactVCore.ImageLength);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    /* Image is only virtually contiguous, translate each page */
    for (Offset = 0; Offset < ReactVCore.ImageLength; Offset += AMD64_PAGE_SIZE)
    {
        ULONG64 Virtual = ReactVCore.ImageBase + Offset;

        Status = Memory->TranslateVirtual((PVOID)(ULONG_PTR)Virtual, &Physical);
        if (!NT_SUCCESS(Status))
        {
            ReactVLogError("No PA for core image VA %I64x\n", Virtual);
            return Status;
        }

        Status = ReactVMapCorePage(Virtual, Physical, 0);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVCreateAddressSpace(VOID)
{
    PREACTV_ADDRESS_SPACE Space = &ReactVCore.Space;
    ULONG64 PoolBase;
    NTSTATUS Status;

    Status = ReactVCore.Loader->Memory->AllocatePages(REACTV_TABLE_POOL_PAGES,
                                                      1,
                                                      ReactVMemoryPageTable,
                                                      &PoolBase);
    if (!NT_SUCCESS(Status))
    {
        ReactVLogError("Table pool allocation failed, %lx\n", Status);
        return Status;
    }

    Space->TablePoolBase = PoolBase;
    Space->TablePoolPages = REACTV_TABLE_POOL_PAGES;
    Space->TablePoolUsed = 0;

    Status = ReactVAllocateTablePage(&Space->Pml4);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The table pool is RAM, so this also maps every table allocated along the way */
    Status = ReactVMapAllMemory();
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVMapCoreImage();
    if (!NT_SUCCESS(Status))
        return Status;

    ReactVLogTrace("Core image %I64x, %I64x bytes, PML4 %I64x\n",
                   ReactVCore.ImageBase,
                   ReactVCore.ImageLength,
                   Space->Pml4);
    ReactVLogTrace("Table pool %I64x, %I64u of %I64u pages used\n",
                   Space->TablePoolBase,
                   Space->TablePoolUsed,
                   Space->TablePoolPages);

    return STATUS_SUCCESS;
}
