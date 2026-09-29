/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Partitions, deposited memory, NPT and VPs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"

/*
 * Nothing here allocates: records live in the core image and all other pages
 * come from memory the root deposited. NPT walks are user accesses, so every
 * entry needs the U bit.
 */

#define REACTV_NPT_FLAGS (AMD64_PTE_PRESENT | AMD64_PTE_WRITE | AMD64_PTE_USER)

/* Keep the SPA already mapped at the GPA */
#define REACTV_KEEP_MAPPED_PAGE ((ULONG64)-1)

/* "Hygo", CPUID leaf 0 EBX for HygonGenuine */
#define REACTV_VENDOR_HYGON     0x6F677948

/* CPUID XSAVE bits and the XsaveFeatures property values */
#define REACTV_CPUID_XSAVE      (1 << 26)
#define REACTV_CPUID_XSAVEOPT   (1 << 0)
#define REACTV_XSAVE_SUPPORTED    1
#define REACTV_XSAVEOPT_SUPPORTED 2

/* FUNCTIONS ******************************************************************/

_Must_inspect_result_
PVOID
NTAPI
ReactVRootPointer(
    _In_ ULONG64 Gpa,
    _In_ ULONG64 Length)
{
    if ((Length == 0) || (Gpa >= ReactVCore.MemoryLimit))
        return NULL;

    if (Length > (ReactVCore.MemoryLimit - Gpa))
        return NULL;

    return (PVOID)(ULONG_PTR)Gpa;
}

/* Slot 0 is always the root */
PREACTV_PARTITION
NTAPI
ReactVRootPartition(VOID)
{
    return &ReactVCore.Partition[0];
}

PREACTV_PARTITION
NTAPI
ReactVPartitionFromId(
    _In_ ULONG64 Id)
{
    ULONG Index;

    if ((Id == HV_PARTITION_ID_SELF) || (Id == REACTV_ROOT_PARTITION_ID))
        return ReactVRootPartition();

    for (Index = 0; Index < REACTV_MAXIMUM_PARTITIONS; Index++)
    {
        if (ReactVCore.Partition[Index].Used &&
            (ReactVCore.Partition[Index].Id == Id))
        {
            return &ReactVCore.Partition[Index];
        }
    }

    return NULL;
}

/* Calling partition; no caller means the root */
PREACTV_PARTITION
NTAPI
ReactVCallerPartition(
    _In_ PREACTV_HYPERCALL Call)
{
    if (Call->Caller != NULL)
        return Call->Caller;

    return ReactVRootPartition();
}

/**
 * @brief
 * Resolves the partition id a hypercall names.
 *
 * @remarks
 * Doorbell calls pass HV_PARTITION_ID_DOORBELL; the doorbell stub loads the
 * real partition id into R9.
 */
PREACTV_PARTITION
NTAPI
ReactVPartitionFromCall(
    _In_ PREACTV_HYPERCALL Call,
    _In_ ULONG64 Id)
{
    if (Id == HV_PARTITION_ID_DOORBELL)
    {
        if (Call->State == NULL)
            return NULL;

        Id = Call->State->Registers.R9;
    }

    return ReactVPartitionFromId(Id);
}

/* Pops and zeroes a page from the deposited list */
static
ULONG64
NTAPI
ReactVTakeDepositedPage(
    _In_ PREACTV_PARTITION Partition)
{
    PULONG64 Page;
    ULONG64 Base = Partition->DepositedList;

    if (Base == 0)
        return 0;

    /* The first ULONG64 of each free page links to the next */
    Page = (PULONG64)(ULONG_PTR)Base;
    Partition->DepositedList = Page[0];
    Partition->DepositedUsed++;

    RtlZeroMemory(Page, AMD64_PAGE_SIZE);

    return Base;
}

/* Pushes a used page back on the deposited list so the root can withdraw it */
static
VOID
NTAPI
ReactVReturnDepositedPage(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Page)
{
    PULONG64 Link;

    if (Page == 0)
        return;

    Link = (PULONG64)(ULONG_PTR)Page;
    Link[0] = Partition->DepositedList;
    Partition->DepositedList = Page;

    if (Partition->DepositedUsed != 0)
        Partition->DepositedUsed--;
}

/* Walks one NPT level, allocating the next table if missing */
static
ULONG64
NTAPI
ReactVNextNestedTable(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 TableBase,
    _In_ ULONG Index)
{
    PULONG64 Table = (PULONG64)(ULONG_PTR)TableBase;
    ULONG64 Next;

    if (Table[Index] & AMD64_PTE_PRESENT)
        return Table[Index] & AMD64_PTE_ADDRESS_MASK;

    Next = ReactVTakeDepositedPage(Partition);
    if (Next == 0)
        return 0;

    Table[Index] = Next | REACTV_NPT_FLAGS;

    return Next;
}

/* Returns the NPT page table for a GPA, building missing levels */
static
ULONG64
NTAPI
ReactVNestedTableForGpa(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa)
{
    ULONG64 TableBase;

    TableBase = ReactVNextNestedTable(Partition, Partition->Npt, AMD64_PXI(Gpa));
    if (TableBase == 0)
        return 0;

    TableBase = ReactVNextNestedTable(Partition, TableBase, AMD64_PPI(Gpa));
    if (TableBase == 0)
        return 0;

    return ReactVNextNestedTable(Partition, TableBase, AMD64_PDI(Gpa));
}

/* Maps one root SPA at a GPA with the requested access rights */
static
ULONG64
NTAPI
ReactVMapGuestPage(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _In_ ULONG64 Spa,
    _In_ ULONG MapFlags)
{
    PULONG64 Table;
    ULONG64 TableBase;
    ULONG64 Entry;
    ULONG Index;

    TableBase = ReactVNestedTableForGpa(Partition, Gpa);
    if (TableBase == 0)
        return HV_STATUS_INSUFFICIENT_MEMORY;

    Table = (PULONG64)(ULONG_PTR)TableBase;
    Index = AMD64_PTI(Gpa);

    /* No page list: permission change only, keep the existing SPA */
    if (Spa == REACTV_KEEP_MAPPED_PAGE)
    {
        Spa = Table[Index] & AMD64_PTE_ADDRESS_MASK;

        /* Never mapped, nothing to change */
        if (Spa == 0)
            return HV_STATUS_SUCCESS;
    }

    Entry = Spa | AMD64_PTE_USER;

    /* No access bits means mapped but not accessible: keep the PFN, clear present so the guest faults to the root */
    if (MapFlags & HV_MAP_GPA_PERMISSIONS_MASK)
    {
        Entry |= AMD64_PTE_PRESENT;

        if (MapFlags & HV_MAP_GPA_WRITABLE)
            Entry |= AMD64_PTE_WRITE;

        if (!(MapFlags & (HV_MAP_GPA_KERNEL_EXECUTABLE | HV_MAP_GPA_USER_EXECUTABLE)))
            Entry |= AMD64_PTE_NO_EXECUTE;
    }

    /* Only count newly present pages */
    if (!(Table[Index] & AMD64_PTE_PRESENT) && (Entry & AMD64_PTE_PRESENT))
        Partition->MappedPages++;

    Table[Index] = Entry;

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVUnmapGuestPage(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa)
{
    PULONG64 Table;
    ULONG64 TableBase;
    ULONG Index;

    /* A missing level means the page was never mapped */
    TableBase = Partition->Npt;

    Table = (PULONG64)(ULONG_PTR)TableBase;
    if (!(Table[AMD64_PXI(Gpa)] & AMD64_PTE_PRESENT))
        return HV_STATUS_SUCCESS;

    TableBase = Table[AMD64_PXI(Gpa)] & AMD64_PTE_ADDRESS_MASK;

    Table = (PULONG64)(ULONG_PTR)TableBase;
    if (!(Table[AMD64_PPI(Gpa)] & AMD64_PTE_PRESENT))
        return HV_STATUS_SUCCESS;

    TableBase = Table[AMD64_PPI(Gpa)] & AMD64_PTE_ADDRESS_MASK;

    Table = (PULONG64)(ULONG_PTR)TableBase;
    if (!(Table[AMD64_PDI(Gpa)] & AMD64_PTE_PRESENT))
        return HV_STATUS_SUCCESS;

    TableBase = Table[AMD64_PDI(Gpa)] & AMD64_PTE_ADDRESS_MASK;

    Table = (PULONG64)(ULONG_PTR)TableBase;
    Index = AMD64_PTI(Gpa);

    if (Table[Index] & AMD64_PTE_PRESENT)
    {
        Table[Index] = 0;
        Partition->MappedPages--;
    }

    return HV_STATUS_SUCCESS;
}

/* Returns a VP's pages to the deposited list */
static
VOID
NTAPI
ReactVReleaseVpPages(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp)
{
    ULONG Index;

    for (Index = 0; Index < HV_VP_STATE_PAGE_TYPES; Index++)
    {
        ReactVReturnDepositedPage(Partition, Vp->StatePage[Index]);
        Vp->StatePage[Index] = 0;
    }

    ReactVReturnDepositedPage(Partition, Vp->StatsPage);
    Vp->StatsPage = 0;

    ReactVReturnDepositedPage(Partition, Vp->FpuState);
    Vp->FpuState = 0;

    ReactVReturnDepositedPage(Partition, Vp->Vmcb);
    Vp->Vmcb = 0;
}

/**
 * @brief
 * Returns the NPT table pages to the deposited list.
 *
 * @remarks
 * Leaf targets are root memory and are not freed.
 */
static
VOID
NTAPI
ReactVReleaseNestedTables(
    _In_ PREACTV_PARTITION Partition)
{
    PULONG64 Top, Parent, Directory;
    ULONG TopIndex, ParentIndex, DirectoryIndex;

    if (Partition->Npt == 0)
        return;

    Top = (PULONG64)(ULONG_PTR)Partition->Npt;

    for (TopIndex = 0; TopIndex <= AMD64_TABLE_INDEX_MASK; TopIndex++)
    {
        if (!(Top[TopIndex] & AMD64_PTE_PRESENT))
            continue;

        Parent = (PULONG64)(ULONG_PTR)(Top[TopIndex] & AMD64_PTE_ADDRESS_MASK);

        for (ParentIndex = 0; ParentIndex <= AMD64_TABLE_INDEX_MASK; ParentIndex++)
        {
            if (!(Parent[ParentIndex] & AMD64_PTE_PRESENT))
                continue;

            /* Large page points at root memory, not a table */
            if (Parent[ParentIndex] & AMD64_PTE_LARGE_PAGE)
                continue;

            Directory = (PULONG64)(ULONG_PTR)(Parent[ParentIndex] & AMD64_PTE_ADDRESS_MASK);

            for (DirectoryIndex = 0;
                 DirectoryIndex <= AMD64_TABLE_INDEX_MASK;
                 DirectoryIndex++)
            {
                if (!(Directory[DirectoryIndex] & AMD64_PTE_PRESENT) ||
                    (Directory[DirectoryIndex] & AMD64_PTE_LARGE_PAGE))
                {
                    continue;
                }

                ReactVReturnDepositedPage(Partition,
                                          Directory[DirectoryIndex] &
                                              AMD64_PTE_ADDRESS_MASK);
            }

            ReactVReturnDepositedPage(Partition, (ULONG64)(ULONG_PTR)Directory);
        }

        ReactVReturnDepositedPage(Partition, (ULONG64)(ULONG_PTR)Parent);
    }

    ReactVReturnDepositedPage(Partition, Partition->Npt);

    Partition->Npt = 0;
    Partition->MappedPages = 0;
}

/**
 * @brief
 * Returns every used page to the deposited list so the root can withdraw them all.
 */
static
VOID
NTAPI
ReactVReleasePartitionPages(
    _In_ PREACTV_PARTITION Partition)
{
    PREACTV_VP Vp;
    ULONG Index;

    /* Tear down all VPs */
    for (Index = 0; Index < REACTV_MAXIMUM_VPS; Index++)
    {
        Vp = &Partition->Vp[Index];
        if (!Vp->Created)
            continue;

        ReactVReleaseVpPages(Partition, Vp);
        RtlZeroMemory(Vp, sizeof(*Vp));
    }

    ReactVReleaseNestedTables(Partition);

    ReactVReturnDepositedPage(Partition, Partition->StatsPage);
    Partition->StatsPage = 0;

    ReactVReturnDepositedPage(Partition, Partition->DoorbellPage);
    Partition->DoorbellPage = 0;
}

/**
 * @brief
 * Translates a GPA through the NPT without building any levels.
 *
 * @param[out] Address
 * SPA of the byte. The core identity maps physical memory, so it is usable as a pointer.
 *
 * @return
 * REACTV_GUEST_PAGE_* state of the mapping.
 */
ULONG
NTAPI
ReactVGuestPage(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _Out_ PULONG64 Address)
{
    PULONG64 Table;
    ULONG64 Entry;
    ULONG64 Page;

    *Address = 0;

    if (Partition->Npt == 0)
        return REACTV_GUEST_PAGE_ABSENT;

    Table = (PULONG64)(ULONG_PTR)Partition->Npt;

    Entry = Table[AMD64_PXI(Gpa)];
    if (!(Entry & AMD64_PTE_PRESENT))
        return REACTV_GUEST_PAGE_ABSENT;

    Table = (PULONG64)(ULONG_PTR)(Entry & AMD64_PTE_ADDRESS_MASK);

    Entry = Table[AMD64_PPI(Gpa)];
    if (!(Entry & AMD64_PTE_PRESENT))
        return REACTV_GUEST_PAGE_ABSENT;

    Table = (PULONG64)(ULONG_PTR)(Entry & AMD64_PTE_ADDRESS_MASK);

    Entry = Table[AMD64_PDI(Gpa)];
    if (!(Entry & AMD64_PTE_PRESENT))
        return REACTV_GUEST_PAGE_ABSENT;

    Table = (PULONG64)(ULONG_PTR)(Entry & AMD64_PTE_ADDRESS_MASK);

    Entry = Table[AMD64_PTI(Gpa)];
    Page = Entry & AMD64_PTE_ADDRESS_MASK;

    /* Never mapped */
    if (Page == 0)
        return REACTV_GUEST_PAGE_ABSENT;

    *Address = Page + (Gpa & AMD64_PAGE_MASK);

    /* Withheld pages are still root memory, so Address stays valid for the root */
    if (!(Entry & AMD64_PTE_PRESENT))
        return REACTV_GUEST_PAGE_WITHHELD;

    if (!(Entry & AMD64_PTE_WRITE))
        return REACTV_GUEST_PAGE_READ_ONLY;

    return REACTV_GUEST_PAGE_PRESENT;
}

/**
 * @brief
 * Finds an ASID no live partition holds.
 *
 * @remarks
 * ASIDs are reused; a new owner flushes the ASID on its first run.
 *
 * @return
 * Zero when all ASIDs are in use.
 */
static
ULONG
NTAPI
ReactVTakeAsid(VOID)
{
    ULONG Asid;

    for (Asid = REACTV_ROOT_ASID + 1; Asid < ReactVCore.SvmAsidCount; Asid++)
    {
        BOOLEAN Held = FALSE;
        ULONG Index;

        for (Index = 1; Index < REACTV_MAXIMUM_PARTITIONS; Index++)
        {
            if (ReactVCore.Partition[Index].Used &&
                (ReactVCore.Partition[Index].Asid == Asid))
            {
                Held = TRUE;
                break;
            }
        }

        if (!Held)
            return Asid;
    }

    return 0;
}

/* HYPERCALLS *****************************************************************/

static
ULONG64
NTAPI
ReactVCreatePartition(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_CREATE_PARTITION Input;
    PREACTV_PARTITION Partition = NULL;
    PULONG64 Output;
    ULONG Index;

    ULONG Asid;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    /* Slot 0 is the root */
    for (Index = 1; Index < REACTV_MAXIMUM_PARTITIONS; Index++)
    {
        if (!ReactVCore.Partition[Index].Used)
        {
            Partition = &ReactVCore.Partition[Index];
            break;
        }
    }

    if (Partition == NULL)
    {
        ReactVLogWarning("Partition table full (%u)\n",
                         REACTV_MAXIMUM_PARTITIONS - 1);
        return HV_STATUS_NO_RESOURCES;
    }

    Asid = ReactVTakeAsid();
    if (Asid == 0)
    {
        ReactVLogWarning("No free ASID (%lu supported)\n",
                         ReactVCore.SvmAsidCount);
        return HV_STATUS_NO_RESOURCES;
    }

    RtlZeroMemory(Partition, sizeof(*Partition));

    Partition->Used = TRUE;
    Partition->Id = ReactVCore.NextPartitionId++;
    Partition->Asid = Asid;
    Partition->CreationFlags = Input->Flags;
    Partition->ProcessorCount = 1;

    /* NPT root is allocated at initialize, from deposited memory */
    Partition->Npt = 0;

    *Output = Partition->Id;

    ReactVLogTrace("Partition %I64u created, flags %I64x, ASID %lu\n",
                   Partition->Id,
                   Partition->CreationFlags,
                   Partition->Asid);

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVInitializePartition(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_PARTITION_ID Input;
    PREACTV_PARTITION Partition;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Partition->Initialized)
        return HV_STATUS_INVALID_PARTITION_STATE;

    /* NPT PML4 comes from the deposited pages */
    if (Partition->Npt == 0)
    {
        Partition->Npt = ReactVTakeDepositedPage(Partition);
        if (Partition->Npt == 0)
            return HV_STATUS_INSUFFICIENT_MEMORY;
    }

    Partition->Initialized = TRUE;

    ReactVLogTrace("Partition %I64u initialized, NPT at %I64x\n",
                   Partition->Id,
                   Partition->Npt);

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVFinalizePartition(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_PARTITION_ID Input;
    PREACTV_PARTITION Partition;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    /* Return all used pages so the root can withdraw them next */
    ReactVReleasePartitionPages(Partition);

    Partition->Finalized = TRUE;

    ReactVLogTrace("Partition %I64u finalized, %I64u of %I64u deposited pages in use\n",
                   Partition->Id,
                   Partition->DepositedUsed,
                   Partition->DepositedPages);

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVDeletePartition(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_PARTITION_ID Input;
    PREACTV_PARTITION Partition;
    ULONG Index;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    /* All VPs must be deleted first */
    for (Index = 0; Index < REACTV_MAXIMUM_VPS; Index++)
    {
        if (Partition->Vp[Index].Created)
            return HV_STATUS_INVALID_PARTITION_STATE;
    }

    ReactVLogTrace("Partition %I64u deleted, %I64u deposited pages not withdrawn\n",
                   Partition->Id,
                   Partition->DepositedPages);

    /* Drop ports that target this partition */
    ReactVReleasePorts(Partition->Id);

    /* Pages not withdrawn are leaked with the record */
    RtlZeroMemory(Partition, sizeof(*Partition));

    return HV_STATUS_SUCCESS;
}

/* Looks up a stored, unhandled property */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVReadRecordedProperty(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG Code,
    _Out_ PULONG64 Value)
{
    ULONG Index;

    for (Index = 0; Index < REACTV_RECORDED_PROPERTIES; Index++)
    {
        if (Partition->Recorded[Index].Code == Code)
        {
            *Value = Partition->Recorded[Index].Value;
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief
 * Stores a property the core does not act on.
 *
 * @return
 * FALSE when the table is full.
 */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVRecordProperty(
    _Inout_ PREACTV_PARTITION Partition,
    _In_ ULONG Code,
    _In_ ULONG64 Value)
{
    ULONG Index;

    for (Index = 0; Index < REACTV_RECORDED_PROPERTIES; Index++)
    {
        if ((Partition->Recorded[Index].Code == Code) ||
            (Partition->Recorded[Index].Code == 0))
        {
            if (Partition->Recorded[Index].Code == 0)
            {
                ReactVLogWarning("Partition property %lx stored only, value %I64x\n",
                                 Code,
                                 Value);
            }

            Partition->Recorded[Index].Code = Code;
            Partition->Recorded[Index].Value = Value;
            return TRUE;
        }
    }

    return FALSE;
}

/* Physical address width, CPUID 0x80000008 EAX[7:0] */
static
ULONG64
NTAPI
ReactVPhysicalAddressWidth(VOID)
{
    int Result[4];

    __cpuidex(Result, 0x80000008, 0);
    return Result[0] & 0xFF;
}

/* Processor properties; children see host CPUID unfiltered, so these report the host */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVProcessorProperty(
    _In_ ULONG PropertyCode,
    _Out_ PULONG64 Output)
{
    int Result[4];

    switch (PropertyCode)
    {
        case HvPartitionPropertyProcessorVendor:
            __cpuidex(Result, 0, 0);

            /* EBX alone distinguishes Hygon from AMD */
            if ((ULONG)Result[1] == REACTV_VENDOR_HYGON)
                *Output = HV_PROCESSOR_VENDOR_HYGON;
            else
                *Output = HV_PROCESSOR_VENDOR_AMD;

            return TRUE;

        case HvPartitionPropertyProcessorFeatures:
        case HvPartitionPropertyProcessorFeaturesBank0:
        case HvPartitionPropertyProcessorFeaturesBank1:
            /* Child CPUID is unfiltered; the caller masks this with what it supports */
            *Output = ~0ULL;
            return TRUE;

        case HvPartitionPropertyProcessorXsaveFeatures:
            *Output = 0;

            __cpuidex(Result, 1, 0);
            if ((Result[2] & REACTV_CPUID_XSAVE) != 0)
            {
                *Output = REACTV_XSAVE_SUPPORTED;

                __cpuidex(Result, 0x0D, 1);
                if ((Result[0] & REACTV_CPUID_XSAVEOPT) != 0)
                    *Output |= REACTV_XSAVEOPT_SUPPORTED;
            }

            return TRUE;

        case HvPartitionPropertyProcessorClFlushSize:
            /* CPUID 1 EBX[15:8], in 8 byte units */
            __cpuidex(Result, 1, 0);
            *Output = (Result[1] >> 8) & 0xFF;
            return TRUE;

        case HvPartitionPropertyPhysicalAddressWidth:
            *Output = ReactVPhysicalAddressWidth();
            return TRUE;

        case HvPartitionPropertyXsaveStates:
            __cpuidex(Result, 0x0D, 0);
            *Output = ((ULONG64)(ULONG)Result[3] << 32) | (ULONG)Result[0];
            return TRUE;

        case HvPartitionPropertyMaxXsaveDataSize:
            __cpuidex(Result, 0x0D, 0);
            *Output = (ULONG)Result[2];
            return TRUE;

        case HvPartitionPropertyProcessorClockFrequency:
            *Output = ReactVCyclesPerSecond();
            return TRUE;

        case HvPartitionPropertySyntheticProcFeatures:
            /* No enlightenments offered to children */
            *Output = 0;
            return TRUE;
    }

    return FALSE;
}

/* Shared by GetPartitionProperty and GetPartitionPropertyEx */
static
ULONG64
NTAPI
ReactVReadPartitionProperty(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG PropertyCode,
    _Out_ PULONG64 Output)
{
    switch (PropertyCode)
    {
        case HvPartitionPropertyPrivilegeFlags:
            *Output = Partition->PrivilegeFlags;
            break;

        case HvPartitionPropertyProcessorCount:
            *Output = Partition->ProcessorCount;
            break;

        case HvPartitionPropertyCpuReserve:
            *Output = Partition->CpuReserve;
            break;

        case HvPartitionPropertyCpuCap:
            *Output = Partition->CpuCap;
            break;

        case HvPartitionPropertyCpuWeight:
            *Output = Partition->CpuWeight;
            break;

        case HvPartitionPropertyGpaSpaceOptions:
            *Output = Partition->GpaSpaceOptions;
            break;

        case HvPartitionPropertyDoorbellPage:
            /* Mapped into the VMM process for hypercalls without a syscall; taken from deposited pages */
            if (Partition == ReactVRootPartition())
                return HV_STATUS_ACCESS_DENIED;

            if (Partition->DoorbellPage == 0)
            {
                Partition->DoorbellPage = ReactVTakeDepositedPage(Partition);
                if (Partition->DoorbellPage == 0)
                    return HV_STATUS_INSUFFICIENT_MEMORY;

                ReactVWriteDoorbellStub(Partition->DoorbellPage, Partition->Id);
            }

            *Output = Partition->DoorbellPage;
            break;

        case HvPartitionPropertyGpaAddressWidth:
            /* Child only; GPA width matches the host physical address width */
            if (Partition == ReactVRootPartition())
                return HV_STATUS_ACCESS_DENIED;

            *Output = ReactVPhysicalAddressWidth();
            break;

        default:
            if (ReactVProcessorProperty(PropertyCode, Output))
                break;

            if (ReactVReadRecordedProperty(Partition, PropertyCode, Output))
                break;

            ReactVLogWarning("Unsupported partition property %lx\n",
                             PropertyCode);
            return HV_STATUS_INVALID_PARAMETER;
    }

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVGetPartitionProperty(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_GET_PARTITION_PROPERTY Input;
    PREACTV_PARTITION Partition;
    PULONG64 Output;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    return ReactVReadPartitionProperty(Partition, Input->PropertyCode, Output);
}

/**
 * @brief
 * Extended form of GetPartitionProperty.
 *
 * @remarks
 * The caller reads the full output buffer, so it is zeroed first. The extra
 * input argument is only used by range properties, none of which are supported.
 */
static
ULONG64
NTAPI
ReactVGetPartitionPropertyEx(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_GET_PARTITION_PROPERTY_EX Input;
    PREACTV_PARTITION Partition;
    PULONG64 Output;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call,
                                   sizeof(*Input),
                                   HV_PARTITION_PROPERTY_EX_OUTPUT);
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    RtlZeroMemory(Output, HV_PARTITION_PROPERTY_EX_OUTPUT);

    return ReactVReadPartitionProperty(Partition, Input->PropertyCode, Output);
}

/**
 * @brief
 * Sets a system wide property.
 *
 * @remarks
 * These tune profiling, debugging and scheduling, which the core lacks, so they
 * are only stored. Failing the call makes the root assume an older hypervisor.
 */
static
ULONG64
NTAPI
ReactVSetSystemProperty(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_SET_SYSTEM_PROPERTY Input;
    ULONG64 Value;
    ULONG Index;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    /* Only the first ULONG64 is kept */
    RtlCopyMemory(&Value, Input->PropertyValue, sizeof(Value));

    ReactVLogTrace("System property %lx stored only, value %I64x\n",
                   Input->PropertyId,
                   Value);

    /* Separate table: system and partition property codes overlap */
    for (Index = 0; Index < REACTV_RECORDED_PROPERTIES; Index++)
    {
        if ((ReactVCore.System[Index].Code == Input->PropertyId) ||
            (ReactVCore.System[Index].Code == 0))
        {
            ReactVCore.System[Index].Code = Input->PropertyId;
            ReactVCore.System[Index].Value = Value;
            return HV_STATUS_SUCCESS;
        }
    }

    /* Table full */
    return HV_STATUS_INSUFFICIENT_MEMORY;
}

static
ULONG64
NTAPI
ReactVSetPartitionProperty(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_SET_PARTITION_PROPERTY Input;
    PREACTV_PARTITION Partition;
    ULONG Index;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    switch (Input->PropertyCode)
    {
        case HvPartitionPropertyPrivilegeFlags:
            /* Only settable before initialize */
            if (Partition->Initialized)
            {
                ReactVLogWarning("Partition %I64u already initialized,"
                                 " privilege flags are fixed\n",
                                 Partition->Id);
                return HV_STATUS_INVALID_PARTITION_STATE;
            }

            Partition->PrivilegeFlags = Input->PropertyValue;
            break;

        case HvPartitionPropertyProcessorCount:
            /*
             * Settable after initialize, but may not drop below an existing VP.
             * Zero means no limit.
             */
            if ((Input->PropertyValue != 0) &&
                (Input->PropertyValue < REACTV_MAXIMUM_VPS))
            {
                for (Index = (ULONG)Input->PropertyValue;
                     Index < REACTV_MAXIMUM_VPS;
                     Index++)
                {
                    if (Partition->Vp[Index].Created)
                        return HV_STATUS_INVALID_PARAMETER;
                }
            }

            Partition->ProcessorCount = Input->PropertyValue;
            break;

        case HvPartitionPropertyGpaSpaceOptions:
            /* 32 bit value */
            if ((Input->PropertyValue >> 32) != 0)
                return HV_STATUS_INVALID_PARAMETER;

            /* GPA access tracking is not supported */
            if ((Input->PropertyValue & HV_GPA_SPACE_ACCESS_TRACKING) != 0)
                return HV_STATUS_INVALID_PARAMETER;

            Partition->GpaSpaceOptions = (ULONG)Input->PropertyValue;
            break;

        case HvPartitionPropertyCpuReserve:
            Partition->CpuReserve = Input->PropertyValue;
            break;

        case HvPartitionPropertyCpuCap:
            Partition->CpuCap = Input->PropertyValue;
            break;

        case HvPartitionPropertyCpuWeight:
            Partition->CpuWeight = Input->PropertyValue;
            break;

        default:
            /* Unhandled policy: store it, since failing aborts the caller */
            if (!ReactVRecordProperty(Partition,
                                      Input->PropertyCode,
                                      Input->PropertyValue))
            {
                ReactVLogWarning("Property table full, cannot store partition property %lx\n",
                                 Input->PropertyCode);
                return HV_STATUS_INSUFFICIENT_MEMORY;
            }

            break;
    }

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVGetPartitionId(
    _In_ PREACTV_HYPERCALL Call)
{
    PULONG64 Output = ReactVHypercallOutput(Call, 0, sizeof(ULONG64));

    if (Output == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    *Output = REACTV_ROOT_PARTITION_ID;

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVDepositMemory(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *RepsCompleted)
{
    PHV_INPUT_DEPOSIT_MEMORY Input;
    PREACTV_PARTITION Partition;
    ULONG64 Index, Page;
    PULONG64 Link;

    *RepsCompleted = 0;

    if (Call->RepCount == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    Input = ReactVHypercallInput(Call,
                                 FIELD_OFFSET(HV_INPUT_DEPOSIT_MEMORY, GpaPageList) +
                                 (Call->RepCount * sizeof(ULONG64)));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    for (Index = 0; Index < Call->RepCount; Index++)
    {
        Page = (Input->GpaPageList[Index] & HV_PAGE_NUMBER_MASK) << AMD64_PAGE_SHIFT;

        Link = ReactVRootPointer(Page, AMD64_PAGE_SIZE);
        if (Link == NULL)
            return HV_STATUS_INVALID_PARAMETER;

        /* Link through the deposited page itself */
        Link[0] = Partition->DepositedList;
        Partition->DepositedList = Page;
        Partition->DepositedPages++;

        *RepsCompleted = Index + 1;
    }

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Returns unused deposited pages to the root.
 *
 * @param[out] RepsCompleted
 * Number of PFNs written.
 *
 * @return
 * HV_STATUS_NO_RESOURCES when the list is empty; the root uses this to stop.
 */
static
ULONG64
NTAPI
ReactVWithdrawMemory(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *RepsCompleted)
{
    PHV_INPUT_WITHDRAW_MEMORY Input;
    PREACTV_PARTITION Partition;
    PULONG64 Output;
    ULONG64 Index, Page;

    *RepsCompleted = 0;

    if (Call->RepCount == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    /* Output is one page of PFNs */
    if (Call->RepCount > (AMD64_PAGE_SIZE / sizeof(ULONG64)))
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call,
                                  sizeof(*Input),
                                  Call->RepCount * sizeof(ULONG64));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    for (Index = 0; Index < Call->RepCount; Index++)
    {
        Page = ReactVTakeDepositedPage(Partition);
        if (Page == 0)
            return HV_STATUS_NO_RESOURCES;

        /* Undo the used count from the take; the page leaves the partition */
        Partition->DepositedUsed--;
        Partition->DepositedPages--;

        Output[Index] = Page >> AMD64_PAGE_SHIFT;
        *RepsCompleted = Index + 1;
    }

    return HV_STATUS_SUCCESS;
}

/* Returns the stats page slot for a stats object */
static
_Must_inspect_result_
ULONG64
NTAPI
ReactVStatsPageOf(
    _In_ PREACTV_PARTITION Partition,
    _In_ PHV_INPUT_MAP_STATS_PAGE Input,
    _Outptr_ PULONG64 *Page)
{
    switch (Input->StatsType)
    {
        case HV_STATS_OBJECT_PARTITION:
            *Page = &Partition->StatsPage;
            return HV_STATUS_SUCCESS;

        case HV_STATS_OBJECT_VP:
            if (Input->VpIndex >= REACTV_MAXIMUM_VPS)
                return HV_STATUS_INVALID_VP_INDEX;

            *Page = &Partition->Vp[Input->VpIndex].StatsPage;
            return HV_STATUS_SUCCESS;

        default:
            ReactVLogWarning("Unsupported stats object type %I64x\n",
                             Input->StatsType);
            return HV_STATUS_INVALID_PARAMETER;
    }
}

/**
 * @brief
 * Maps the partition or VP stats page for the root.
 *
 * @remarks
 * No counters are maintained yet, so every counter reads zero.
 */
static
ULONG64
NTAPI
ReactVMapStatsPage(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_MAP_STATS_PAGE Input;
    PREACTV_PARTITION Partition;
    PULONG64 Output;
    PULONG64 Page;
    ULONG64 Status;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    Status = ReactVStatsPageOf(Partition, Input, &Page);
    if (Status != HV_STATUS_SUCCESS)
        return Status;

    /* VP must exist */
    if ((Input->StatsType == HV_STATS_OBJECT_VP) &&
        !Partition->Vp[Input->VpIndex].Created)
    {
        return HV_STATUS_INVALID_VP_INDEX;
    }

    /* Allocated from deposited pages on first map */
    if (*Page == 0)
    {
        *Page = ReactVTakeDepositedPage(Partition);
        if (*Page == 0)
            return HV_STATUS_INSUFFICIENT_MEMORY;
    }

    *Output = *Page >> AMD64_PAGE_SHIFT;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Maps a VP state page shared between the root and the core.
 *
 * @remarks
 * Allocated from deposited pages on first map; later calls return the same page.
 */
static
ULONG64
NTAPI
ReactVMapVpStatePage(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_MAP_VP_STATE_PAGE Input;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;
    PULONG64 Output;
    PULONG64 Page;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Input->VpIndex >= REACTV_MAXIMUM_VPS)
        return HV_STATUS_INVALID_VP_INDEX;

    Vp = &Partition->Vp[Input->VpIndex];
    if (!Vp->Created)
        return HV_STATUS_INVALID_VP_INDEX;

    if (Input->StateType >= HV_VP_STATE_PAGE_TYPES)
    {
        ReactVLogWarning("Unsupported VP state page type %u\n",
                         Input->StateType);
        return HV_STATUS_INVALID_PARAMETER;
    }

    Page = &Vp->StatePage[Input->StateType];

    if (*Page == 0)
    {
        *Page = ReactVTakeDepositedPage(Partition);
        if (*Page == 0)
            return HV_STATUS_INSUFFICIENT_MEMORY;
    }

    *Output = *Page >> AMD64_PAGE_SHIFT;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Installs or removes an intercept for a child partition.
 *
 * @remarks
 * A zero access mask removes it. Types that always exit to the root are
 * accepted with no change.
 */
static
ULONG64
NTAPI
ReactVInstallIntercept(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_INSTALL_INTERCEPT Input;
    PREACTV_PARTITION Partition;
    PREACTV_INTERCEPTS Intercepts;
    ULONG Leaf;
    ULONG Index;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    Intercepts = &Partition->Intercepts;

    switch (Input->InterceptType)
    {
        case HvInterceptTypeX64GlobalCpuid:
            Intercepts->EveryCpuid = (Input->AccessTypeMask != 0);
            return HV_STATUS_SUCCESS;

        case HvInterceptTypeX64Cpuid:
            Leaf = (ULONG)Input->InterceptParameter;

            ReactVLogTrace("Partition %I64u intercepts CPUID %lx\n",
                           Partition->Id,
                           Leaf);

            for (Index = 0; Index < Intercepts->CpuidCount; Index++)
            {
                if (Intercepts->CpuidLeaf[Index] == Leaf)
                    break;
            }

            if (Input->AccessTypeMask == 0)
            {
                /* Swap remove */
                if (Index < Intercepts->CpuidCount)
                {
                    Intercepts->CpuidCount--;
                    Intercepts->CpuidLeaf[Index] =
                        Intercepts->CpuidLeaf[Intercepts->CpuidCount];
                }

                return HV_STATUS_SUCCESS;
            }

            if (Index < Intercepts->CpuidCount)
                return HV_STATUS_SUCCESS;

            /* Table full: fall back to intercepting every leaf */
            if (Intercepts->CpuidCount == REACTV_CPUID_INTERCEPTS)
            {
                Intercepts->EveryCpuid = TRUE;
                return HV_STATUS_SUCCESS;
            }

            Intercepts->CpuidLeaf[Intercepts->CpuidCount++] = Leaf;
            return HV_STATUS_SUCCESS;

        case HvInterceptTypeX64Msr:
            Intercepts->EveryMsr = (Input->AccessTypeMask != 0);
            return HV_STATUS_SUCCESS;

        case HvInterceptTypeHypercall:
            Intercepts->EveryHypercall = (Input->AccessTypeMask != 0);
            return HV_STATUS_SUCCESS;

        case HvInterceptTypeException:
            if (Input->InterceptParameter >= 64)
                return HV_STATUS_INVALID_PARAMETER;

            if (Input->AccessTypeMask != 0)
                Intercepts->ExceptionVectors |= 1ULL << Input->InterceptParameter;
            else
                Intercepts->ExceptionVectors &= ~(1ULL << Input->InterceptParameter);

            return HV_STATUS_SUCCESS;

        default:
            break;
    }

    /* Remaining types already exit to the root unconditionally */
    ReactVLogTrace("Intercept type %lu, mask %lx, always exits, ignored\n",
                   Input->InterceptType,
                   Input->AccessTypeMask);

    return HV_STATUS_SUCCESS;
}

/* Unmaps a stats page and returns it to the deposited list */
static
ULONG64
NTAPI
ReactVUnmapStatsPage(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_MAP_STATS_PAGE Input;
    PREACTV_PARTITION Partition;
    PULONG64 Page;
    ULONG64 Status;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    Status = ReactVStatsPageOf(Partition, Input, &Page);
    if (Status != HV_STATUS_SUCCESS)
        return Status;

    ReactVReturnDepositedPage(Partition, *Page);
    *Page = 0;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Unmaps a VP state page and returns it to the deposited list.
 */
static
ULONG64
NTAPI
ReactVUnmapVpStatePage(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_MAP_VP_STATE_PAGE Input;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Input->VpIndex >= REACTV_MAXIMUM_VPS)
        return HV_STATUS_INVALID_VP_INDEX;

    if (Input->StateType >= HV_VP_STATE_PAGE_TYPES)
        return HV_STATUS_INVALID_PARAMETER;

    Vp = &Partition->Vp[Input->VpIndex];

    ReactVReturnDepositedPage(Partition, Vp->StatePage[Input->StateType]);
    Vp->StatePage[Input->StateType] = 0;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Prebuilds NPT levels for a GPA range so a later map cannot run out of pages.
 *
 * @param[out] RepsCompleted
 * Number of pages prepared.
 */
static
ULONG64
NTAPI
ReactVPrecommitGpaPages(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *RepsCompleted)
{
    PHV_INPUT_PRECOMMIT_GPA_PAGES Input;
    PREACTV_PARTITION Partition;
    ULONG64 Index, Gpa;

    *RepsCompleted = 0;

    if (Call->RepCount == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Partition->Npt == 0)
        return HV_STATUS_INVALID_PARTITION_STATE;

    for (Index = 0; Index < Call->RepCount; Index++)
    {
        Gpa = ((Input->StartGpaPage & HV_PAGE_NUMBER_MASK) + Index) <<
              AMD64_PAGE_SHIFT;

        if (ReactVNestedTableForGpa(Partition, Gpa) == 0)
            return HV_STATUS_INSUFFICIENT_MEMORY;

        *RepsCompleted = Index + 1;
    }

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Uncommits a GPA range.
 *
 * @remarks
 * No-op: NPT table pages are kept until the partition is finalized, and leaf
 * entries are cleared by unmap.
 */
static
ULONG64
NTAPI
ReactVUncommitGpaPages(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *RepsCompleted)
{
    PHV_INPUT_UNCOMMIT_GPA_PAGES Input;
    PREACTV_PARTITION Partition;

    *RepsCompleted = 0;

    if (Call->RepCount == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    *RepsCompleted = Call->RepCount;

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVGetMemoryBalance(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_PARTITION_ID Input;
    PHV_OUTPUT_GET_MEMORY_BALANCE Output;
    PREACTV_PARTITION Partition;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    Output->PagesAvailable = Partition->DepositedPages - Partition->DepositedUsed;
    Output->PagesInUse = Partition->DepositedUsed;

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVMapGpaPages(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *RepsCompleted)
{
    PHV_INPUT_MAP_GPA_PAGES Input;
    PREACTV_PARTITION Partition;
    ULONG64 Index, Gpa, Spa, Status;
    ULONG MapFlags;
    BOOLEAN Listed;

    *RepsCompleted = 0;

    if (Call->RepCount == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    /* Permission only calls carry no page list */
    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Listed = (Input->MapFlags & HV_MAP_GPA_NO_PAGE_LIST) == 0;

    if (Listed)
    {
        Input = ReactVHypercallInput(Call,
                                     FIELD_OFFSET(HV_INPUT_MAP_GPA_PAGES, SourcePageList) +
                                     (Call->RepCount * sizeof(ULONG64)));
        if (Input == NULL)
            return HV_STATUS_INVALID_PARAMETER;
    }

    Partition = ReactVPartitionFromCall(Call, Input->TargetPartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Partition->Npt == 0)
        return HV_STATUS_INVALID_PARTITION_STATE;

    /* Only access rights are honored; other map flags are hints and ignored */
    MapFlags = Input->MapFlags & HV_MAP_GPA_PERMISSIONS_MASK;


    for (Index = 0; Index < Call->RepCount; Index++)
    {
        Gpa = ((Input->TargetGpaBase & HV_PAGE_NUMBER_MASK) + Index) <<
              AMD64_PAGE_SHIFT;
        if (Listed)
        {
            Spa = (Input->SourcePageList[Index] & HV_PAGE_NUMBER_MASK) <<
                  AMD64_PAGE_SHIFT;

            /* Source must be root memory */
            if (ReactVRootPointer(Spa, AMD64_PAGE_SIZE) == NULL)
                return HV_STATUS_INVALID_PARAMETER;
        }
        else
        {
            Spa = REACTV_KEEP_MAPPED_PAGE;
        }

        Status = ReactVMapGuestPage(Partition, Gpa, Spa, MapFlags);
        if (Status != HV_STATUS_SUCCESS)
            return Status;

        *RepsCompleted = Index + 1;
    }

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVUnmapGpaPages(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *RepsCompleted)
{
    PHV_INPUT_UNMAP_GPA_PAGES Input;
    PREACTV_PARTITION Partition;
    ULONG64 Index, Status;

    *RepsCompleted = 0;

    if (Call->RepCount == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->TargetPartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Partition->Npt == 0)
        return HV_STATUS_INVALID_PARTITION_STATE;

    for (Index = 0; Index < Call->RepCount; Index++)
    {
        Status = ReactVUnmapGuestPage(Partition,
                                     (Input->TargetGpaBase + Index) << AMD64_PAGE_SHIFT);
        if (Status != HV_STATUS_SUCCESS)
            return Status;

        *RepsCompleted = Index + 1;
    }

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVCreateVp(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_CREATE_VP Input;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (!Partition->Initialized)
        return HV_STATUS_INVALID_PARTITION_STATE;

    /* Hard limit, independent of ProcessorCount */
    if (Input->VpIndex >= REACTV_MAXIMUM_VPS)
        return HV_STATUS_INVALID_VP_INDEX;

    if ((Partition->ProcessorCount != 0) &&
        (Input->VpIndex >= Partition->ProcessorCount))
    {
        return HV_STATUS_INVALID_VP_INDEX;
    }

    Vp = &Partition->Vp[Input->VpIndex];
    if (Vp->Created)
        return HV_STATUS_INVALID_VP_INDEX;

    RtlZeroMemory(Vp, sizeof(*Vp));

    /* VMCB and FPU state come from deposited pages */
    Vp->Vmcb = ReactVTakeDepositedPage(Partition);
    if (Vp->Vmcb == 0)
        return HV_STATUS_INSUFFICIENT_MEMORY;

    Vp->FpuState = ReactVTakeDepositedPage(Partition);
    if (Vp->FpuState == 0)
    {
        ReactVReturnDepositedPage(Partition, Vp->Vmcb);
        Vp->Vmcb = 0;
        return HV_STATUS_INSUFFICIENT_MEMORY;
    }

    Vp->Index = Input->VpIndex;
    Vp->Created = TRUE;

    ReactVChildBuildVmcb(Partition, Vp);

    ReactVLogTrace("Partition %I64u VP %lu created, VMCB %I64x\n",
                   Partition->Id,
                   Vp->Index,
                   Vp->Vmcb);

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVDeleteVp(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_DELETE_VP Input;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Input->VpIndex >= REACTV_MAXIMUM_VPS)
        return HV_STATUS_INVALID_VP_INDEX;

    Vp = &Partition->Vp[Input->VpIndex];
    if (!Vp->Created)
        return HV_STATUS_INVALID_VP_INDEX;

    /* Return VP pages to the deposited list */
    ReactVReleaseVpPages(Partition, Vp);

    RtlZeroMemory(Vp, sizeof(*Vp));

    return HV_STATUS_SUCCESS;
}


/* Resolves the target VP of a register hypercall */
static
ULONG64
NTAPI
ReactVVpFromInput(
    _In_ PREACTV_HYPERCALL Call,
    _In_ PHV_INPUT_VP_REGISTERS Input,
    _Outptr_ PREACTV_PARTITION *Partition,
    _Outptr_ PREACTV_VP *Vp)
{
    PREACTV_PARTITION Found;

    Found = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Found == NULL) || (Found == ReactVRootPartition()))
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Input->VpIndex >= REACTV_MAXIMUM_VPS)
        return HV_STATUS_INVALID_VP_INDEX;

    if (!Found->Vp[Input->VpIndex].Created)
        return HV_STATUS_INVALID_VP_INDEX;

    *Partition = Found;
    *Vp = &Found->Vp[Input->VpIndex];

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVGetVpRegisters(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *RepsCompleted)
{
    PHV_INPUT_VP_REGISTERS Input;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;
    PULONG Names;
    PHV_REGISTER_VALUE Values;
    ULONG64 Index, Status;

    *RepsCompleted = 0;

    if (Call->RepCount == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    Input = ReactVHypercallInput(Call,
                                 sizeof(*Input) + (Call->RepCount * sizeof(ULONG)));
    Values = ReactVHypercallOutput(Call,
                                   sizeof(*Input) + (Call->RepCount * sizeof(ULONG)),
                                   Call->RepCount * sizeof(HV_REGISTER_VALUE));
    if ((Input == NULL) || (Values == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Status = ReactVVpFromInput(Call, Input, &Partition, &Vp);
    if (Status != HV_STATUS_SUCCESS)
        return Status;

    /* One register name per rep follows the header */
    Names = (PULONG)(Input + 1);

    for (Index = 0; Index < Call->RepCount; Index++)
    {
        Status = ReactVChildReadRegister(Vp, Names[Index], &Values[Index]);
        if (Status != HV_STATUS_SUCCESS)
            return Status;

        *RepsCompleted = Index + 1;
    }

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVSetVpRegisters(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *RepsCompleted)
{
    PHV_INPUT_VP_REGISTERS Input;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;
    PHV_REGISTER_ASSOC Elements;
    ULONG64 Index, Status;
    BOOLEAN Held;

    *RepsCompleted = 0;

    if (Call->RepCount == 0)
        return HV_STATUS_INVALID_HYPERCALL_INPUT;

    Input = ReactVHypercallInput(Call,
                                 sizeof(*Input) +
                                 (Call->RepCount * sizeof(HV_REGISTER_ASSOC)));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Status = ReactVVpFromInput(Call, Input, &Partition, &Vp);
    if (Status != HV_STATUS_SUCCESS)
        return Status;

    Elements = (PHV_REGISTER_ASSOC)(Input + 1);
    Held = ReactVVpHeld(Vp);

    for (Index = 0; Index < Call->RepCount; Index++)
    {
        Status = ReactVChildWriteRegister(Vp,
                                         Elements[Index].Name,
                                         &Elements[Index].Value);
        if (Status != HV_STATUS_SUCCESS)
            return Status;

        *RepsCompleted = Index + 1;
    }

    /* Wake a root thread parked on a VP that was just released */
    if (Held && !ReactVVpHeld(Vp))
        ReactVSynicSignalRunnable(Partition->Id, Vp->Index);

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVDispatchVp(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_DISPATCH_VP Input;
    PHV_OUTPUT_DISPATCH_VP Output;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;
    ULONG64 ExitReason = 0;
    ULONG64 Status;
    BOOLEAN Message = FALSE;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
    {
        return HV_STATUS_INVALID_PARTITION_ID;
    }

    if (Input->VpIndex >= REACTV_MAXIMUM_VPS)
        return HV_STATUS_INVALID_VP_INDEX;

    Vp = &Partition->Vp[Input->VpIndex];
    if (!Vp->Created)
        return HV_STATUS_INVALID_VP_INDEX;

    if (Partition->Npt == 0)
        return HV_STATUS_INVALID_PARTITION_STATE;

    Output->DispatchEvent = 0;

    /*
     * Held VPs do not run. This includes APs still waiting for SIPI, whose
     * register state is not valid yet. Report blocked to the root.
     */
    if (ReactVVpHeld(Vp))
    {
        if (!Vp->ToldBlocked)
        {
            Vp->ToldBlocked = TRUE;

            ReactVLogTrace("Partition %I64u VP %lu held, "
                           "dispatch returns blocked\n",
                           Partition->Id,
                           Vp->Index);
        }

        Output->DispatchState = HV_VP_DISPATCH_STATE_BLOCKED;
        return HV_STATUS_SUCCESS;
    }

    Vp->ToldBlocked = FALSE;

    Status = ReactVChildDispatch(Partition, Vp, &ExitReason, &Message);

    Output->DispatchState = HV_VP_DISPATCH_STATE_READY;

    /* The root only reads the message page when an intercept event is set */
    if (Message)
        Output->DispatchEvent = HV_VP_DISPATCH_EVENT_INTERCEPT;

    return Status;
}

/* THE DISPATCHER *************************************************************/

VOID
NTAPI
ReactVPartitionInitialize(VOID)
{
    PREACTV_PARTITION Root = ReactVRootPartition();

    RtlZeroMemory(Root, sizeof(*Root));

    /* Root has a partition record so calls naming it resolve like a child; no NPT, its GPAs are SPAs */
    Root->Used = TRUE;
    Root->Initialized = TRUE;
    Root->Id = REACTV_ROOT_PARTITION_ID;
    Root->Asid = REACTV_ROOT_ASID;
    Root->ProcessorCount = 1;

    ReactVCore.NextPartitionId = REACTV_ROOT_PARTITION_ID + 1;
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVPartitionHypercall(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *Status,
    _Out_ ULONG64 *RepsCompleted)
{
    *RepsCompleted = 0;

    switch (Call->Code)
    {
        case HvCallCreatePartition:
            *Status = ReactVCreatePartition(Call);
            return TRUE;

        case HvCallInitializePartition:
            *Status = ReactVInitializePartition(Call);
            return TRUE;

        case HvCallFinalizePartition:
            *Status = ReactVFinalizePartition(Call);
            return TRUE;

        case HvCallDeletePartition:
            *Status = ReactVDeletePartition(Call);
            return TRUE;

        case HvCallGetPartitionProperty:
            *Status = ReactVGetPartitionProperty(Call);
            return TRUE;

        case HvCallGetPartitionPropertyEx:
            *Status = ReactVGetPartitionPropertyEx(Call);
            return TRUE;

        case HvCallSetSystemProperty:
            *Status = ReactVSetSystemProperty(Call);
            return TRUE;

        case HvCallSetPartitionProperty:
            *Status = ReactVSetPartitionProperty(Call);
            return TRUE;

        case HvCallGetPartitionId:
            *Status = ReactVGetPartitionId(Call);
            return TRUE;

        case HvCallDepositMemory:
            *Status = ReactVDepositMemory(Call, RepsCompleted);
            return TRUE;

        case HvCallMapVpStatePage:
            *Status = ReactVMapVpStatePage(Call);
            return TRUE;

        case HvCallInstallIntercept:
            *Status = ReactVInstallIntercept(Call);
            return TRUE;

        case HvCallUnmapStatsPage:
            *Status = ReactVUnmapStatsPage(Call);
            return TRUE;

        case HvCallUnmapVpStatePage:
            *Status = ReactVUnmapVpStatePage(Call);
            return TRUE;

        case HvCallMapStatsPage:
            *Status = ReactVMapStatsPage(Call);
            return TRUE;

        case HvCallPrecommitGpaPages:
            *Status = ReactVPrecommitGpaPages(Call, RepsCompleted);
            return TRUE;

        case HvCallUncommitGpaPages:
            *Status = ReactVUncommitGpaPages(Call, RepsCompleted);
            return TRUE;

        case HvCallWithdrawMemory:
            *Status = ReactVWithdrawMemory(Call, RepsCompleted);
            return TRUE;

        case HvCallGetMemoryBalance:
            *Status = ReactVGetMemoryBalance(Call);
            return TRUE;

        case HvCallMapGpaPages:
            *Status = ReactVMapGpaPages(Call, RepsCompleted);
            return TRUE;

        case HvCallUnmapGpaPages:
            *Status = ReactVUnmapGpaPages(Call, RepsCompleted);
            return TRUE;

        case HvCallCreateVp:
            *Status = ReactVCreateVp(Call);
            return TRUE;

        case HvCallDeleteVp:
            *Status = ReactVDeleteVp(Call);
            return TRUE;

        case HvCallGetVpRegisters:
            *Status = ReactVGetVpRegisters(Call, RepsCompleted);
            return TRUE;

        case HvCallSetVpRegisters:
            *Status = ReactVSetVpRegisters(Call, RepsCompleted);
            return TRUE;

        case HvCallDispatchVp:
            *Status = ReactVDispatchVp(Call);
            return TRUE;

        default:
            break;
    }

    return FALSE;
}
