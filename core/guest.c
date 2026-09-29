/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Guest memory access and GVA translation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"

/*
 * GVA walks read each guest table through the partition's GPA map. Unpaged,
 * 32-bit, PAE and long mode paging are handled, since a translation can be
 * requested at any point in guest boot.
 */

/* Guest PTE bits (not NPT format) */
#define REACTV_GUEST_PTE_PRESENT    0x0000000000000001ULL
#define REACTV_GUEST_PTE_WRITE      0x0000000000000002ULL
#define REACTV_GUEST_PTE_USER       0x0000000000000004ULL
#define REACTV_GUEST_PTE_LARGE      0x0000000000000080ULL
#define REACTV_GUEST_PTE_NO_EXECUTE 0x8000000000000000ULL

/* PFN field for 64-bit and 32-bit entries */
#define REACTV_GUEST_PTE_ADDRESS    0x000FFFFFFFFFF000ULL
#define REACTV_GUEST_PTE_ADDRESS_32 0x00000000FFFFF000ULL

/* Table index shift and masks */
#define REACTV_GUEST_LEVEL_SHIFT_32 22
#define REACTV_GUEST_INDEX_MASK_32  0x3FF
#define REACTV_GUEST_INDEX_MASK     0x1FF

/* FUNCTIONS ******************************************************************/

static
PREACTV_VMCB
NTAPI
ReactVGuestVmcb(
    _In_ PREACTV_VP Vp)
{
    return (PREACTV_VMCB)(ULONG_PTR)Vp->Vmcb;
}

/**
 * @brief
 * Reads guest physical memory.
 *
 * @return
 * FALSE if the GPA is unmapped or the range crosses a page.
 */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVGuestRead(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _Out_writes_bytes_all_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    ULONG64 Address;

    if (((Gpa & AMD64_PAGE_MASK) + Length) > AMD64_PAGE_SIZE)
        return FALSE;

    if (ReactVGuestPage(Partition, Gpa, &Address) == REACTV_GUEST_PAGE_ABSENT)
        return FALSE;

    RtlCopyMemory(Buffer, (PVOID)(ULONG_PTR)Address, Length);

    return TRUE;
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVGuestWrite(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    ULONG64 Address;

    if (((Gpa & AMD64_PAGE_MASK) + Length) > AMD64_PAGE_SIZE)
        return FALSE;

    if (ReactVGuestPage(Partition, Gpa, &Address) == REACTV_GUEST_PAGE_ABSENT)
        return FALSE;

    RtlCopyMemory((PVOID)(ULONG_PTR)Address, Buffer, Length);

    return TRUE;
}

/**
 * @brief
 * Returns a host pointer to a guest physical range.
 *
 * @return
 * NULL if the GPA is unmapped, Length is zero, or the range crosses a page.
 */
PVOID
NTAPI
ReactVGuestPointer(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _In_ ULONG64 Length)
{
    ULONG64 Address;

    if ((Length == 0) || (((Gpa & AMD64_PAGE_MASK) + Length) > AMD64_PAGE_SIZE))
        return NULL;

    if (ReactVGuestPage(Partition, Gpa, &Address) == REACTV_GUEST_PAGE_ABSENT)
        return NULL;

    return (PVOID)(ULONG_PTR)Address;
}

/* Reads a 4 or 8 byte guest page table entry */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVReadEntry(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 TableGpa,
    _In_ ULONG Index,
    _In_ BOOLEAN Wide,
    _Out_ PULONG64 Entry)
{
    ULONG Narrow;

    if (Wide)
        return ReactVGuestRead(Partition, TableGpa + (Index * 8), Entry, sizeof(*Entry));

    if (!ReactVGuestRead(Partition, TableGpa + (Index * 4), &Narrow, sizeof(Narrow)))
        return FALSE;

    *Entry = Narrow;

    return TRUE;
}

/* Checks the requested write/execute access against the accumulated PTE flags */
static
_Must_inspect_result_
ULONG
NTAPI
ReactVEntryAllows(
    _In_ ULONG64 Flags,
    _In_ ULONG64 Wanted)
{
    if ((Wanted & HV_TRANSLATE_GVA_VALIDATE_WRITE) && !(Flags & REACTV_GUEST_PTE_WRITE))
        return HvTranslateGvaPrivilegeViolation;

    if ((Wanted & HV_TRANSLATE_GVA_VALIDATE_EXECUTE) &&
        (Flags & REACTV_GUEST_PTE_NO_EXECUTE))
    {
        return HvTranslateGvaPrivilegeViolation;
    }

    return HvTranslateGvaSuccess;
}

/**
 * @brief
 * Translates a GVA to a GPA page using the VP's current paging mode.
 *
 * @param[in] Wanted
 * HV_TRANSLATE_GVA_VALIDATE_* flags to check against the PTEs.
 *
 * @param[out] GpaPage
 * Resulting GPA page. On HvTranslateGvaGpaUnmapped, the unmapped table page so
 * the root can back it.
 *
 * @return
 * HV_TRANSLATE_GVA_RESULT_CODE value.
 */
static
ULONG
NTAPI
ReactVFollowGuestAddress(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG64 Gva,
    _In_ ULONG64 Wanted,
    _Out_ PULONG64 GpaPage)
{
    PREACTV_VMCB Vmcb = ReactVGuestVmcb(Vp);
    ULONG64 Table;
    ULONG64 Entry;
    ULONG64 Flags;
    ULONG64 Page;
    ULONG Index;
    ULONG Level;
    ULONG Levels;
    BOOLEAN Wide;

    *GpaPage = 0;

    /* Paging off: GVA == GPA */
    if (!(Vmcb->Save.Cr0 & AMD64_CR0_PG))
    {
        *GpaPage = Gva >> AMD64_PAGE_SHIFT;
        return HvTranslateGvaSuccess;
    }

    if (Vmcb->Save.Efer & AMD64_EFER_LMA)
    {
        Levels = 4;
        Wide = TRUE;
        Table = Vmcb->Save.Cr3 & REACTV_GUEST_PTE_ADDRESS;
    }
    else if (Vmcb->Save.Cr4 & AMD64_CR4_PAE)
    {
        Levels = 3;
        Wide = TRUE;

        /* PAE CR3 points at a 32 byte aligned, 4 entry PDPT */
        Table = Vmcb->Save.Cr3 & ~0x1FULL;
    }
    else
    {
        Levels = 2;
        Wide = FALSE;
        Table = Vmcb->Save.Cr3 & REACTV_GUEST_PTE_ADDRESS_32;
    }

    Flags = REACTV_GUEST_PTE_WRITE | REACTV_GUEST_PTE_USER;

    for (Level = Levels; Level > 0; Level--)
    {
        if (Wide)
        {
            Index = (ULONG)((Gva >> (AMD64_PAGE_SHIFT + ((Level - 1) * 9))) &
                            REACTV_GUEST_INDEX_MASK);
        }
        else
        {
            Index = (ULONG)((Gva >> (AMD64_PAGE_SHIFT + ((Level - 1) * 10))) &
                            REACTV_GUEST_INDEX_MASK_32);
        }

        /* Table page not mapped in the GPA space: return it as unmapped */
        if (!ReactVReadEntry(Partition, Table, Index, Wide, &Entry))
        {
            *GpaPage = Table >> AMD64_PAGE_SHIFT;
            return HvTranslateGvaGpaUnmapped;
        }

        if (!(Entry & REACTV_GUEST_PTE_PRESENT))
            return HvTranslateGvaPageNotPresent;

        /* W and U are ANDed across levels, NX is ORed */
        Flags &= Entry | ~(REACTV_GUEST_PTE_WRITE | REACTV_GUEST_PTE_USER);
        Flags |= Entry & REACTV_GUEST_PTE_NO_EXECUTE;

        Page = Entry & (Wide ? REACTV_GUEST_PTE_ADDRESS : REACTV_GUEST_PTE_ADDRESS_32);

        /* Large page terminates the walk */
        if ((Level > 1) && (Entry & REACTV_GUEST_PTE_LARGE))
        {
            ULONG Bits = AMD64_PAGE_SHIFT + ((Level - 1) * (Wide ? 9 : 10));

            /* Large page base plus the GVA's offset within it */
            Page &= ~((1ULL << Bits) - 1);
            Page |= Gva & ((1ULL << Bits) - 1) & ~(ULONG64)AMD64_PAGE_MASK;

            *GpaPage = Page >> AMD64_PAGE_SHIFT;
            return ReactVEntryAllows(Flags, Wanted);
        }

        Table = Page;
    }

    *GpaPage = Table >> AMD64_PAGE_SHIFT;

    return ReactVEntryAllows(Flags, Wanted);
}

/**
 * @brief
 * Translates a root VA to a host pointer.
 *
 * @param[in] Cr3
 * Root CR3 to walk.
 *
 * @remarks
 * The root runs without NPT, so only its own 4 level paging is walked.
 *
 * @return
 * NULL if the VA is not mapped or the range crosses a page.
 */
PVOID
NTAPI
ReactVRootTranslate(
    _In_ ULONG64 Cr3,
    _In_ ULONG64 Va,
    _In_ ULONG64 Length)
{
    ULONG64 Table;
    ULONG64 Entry;
    ULONG Level;

    if ((Length == 0) || (((Va & AMD64_PAGE_MASK) + Length) > AMD64_PAGE_SIZE))
        return NULL;

    Table = Cr3 & REACTV_GUEST_PTE_ADDRESS;

    for (Level = 4; Level > 0; Level--)
    {
        PULONG64 Entries = (PULONG64)(ULONG_PTR)Table;
        ULONG Index = (ULONG)((Va >> (AMD64_PAGE_SHIFT + ((Level - 1) * 9))) &
                              REACTV_GUEST_INDEX_MASK);

        if (ReactVRootPointer(Table, AMD64_PAGE_SIZE) == NULL)
            return NULL;

        Entry = Entries[Index];
        if (!(Entry & REACTV_GUEST_PTE_PRESENT))
            return NULL;

        Table = Entry & REACTV_GUEST_PTE_ADDRESS;

        /* Large page terminates the walk */
        if ((Level > 1) && (Entry & REACTV_GUEST_PTE_LARGE))
        {
            ULONG Bits = AMD64_PAGE_SHIFT + ((Level - 1) * 9);

            Table &= ~((1ULL << Bits) - 1);
            Table |= Va & ((1ULL << Bits) - 1) & ~(ULONG64)AMD64_PAGE_MASK;
            break;
        }
    }

    return ReactVRootPointer(Table + (Va & AMD64_PAGE_MASK), Length);
}

/* HYPERCALLS *****************************************************************/

/**
 * @brief
 * HvCallTranslateVirtualAddressEx for a child VP.
 */
static
ULONG64
NTAPI
ReactVTranslateVirtualAddress(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_TRANSLATE_VIRTUAL_ADDRESS Input;
    PHV_OUTPUT_TRANSLATE_VIRTUAL_ADDRESS Output;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;
    ULONG64 GpaPage = 0;

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

    RtlZeroMemory(Output, sizeof(*Output));

    Output->ResultCode = ReactVFollowGuestAddress(Partition,
                                                  Vp,
                                                  Input->GvaPage << AMD64_PAGE_SHIFT,
                                                  Input->ControlFlags,
                                                  &GpaPage);
    Output->CacheType = REACTV_CACHE_WRITE_BACK;
    Output->GpaPage = GpaPage;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * HvCallReadGpa.
 *
 * @remarks
 * Withheld pages are still readable. Only absent GPAs fail, with
 * HvAccessGpaUnmapped in the output.
 */
static
ULONG64
NTAPI
ReactVReadGpa(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_ACCESS_GPA Input;
    PHV_OUTPUT_ACCESS_GPA Output;
    PREACTV_PARTITION Partition;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    if ((Input->ByteCount == 0) || (Input->ByteCount > HV_ACCESS_GPA_MAX_BYTES))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    RtlZeroMemory(Output, sizeof(*Output));

    if (!ReactVGuestRead(Partition, Input->BaseGpa, Output->Data, Input->ByteCount))
    {
        Output->ResultCode = HvAccessGpaUnmapped;
        return HV_STATUS_SUCCESS;
    }

    Output->ResultCode = HvAccessGpaSuccess;

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVWriteGpa(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_ACCESS_GPA Input;
    PHV_OUTPUT_ACCESS_GPA Output;
    PREACTV_PARTITION Partition;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    if ((Input->ByteCount == 0) || (Input->ByteCount > HV_ACCESS_GPA_MAX_BYTES))
        return HV_STATUS_INVALID_PARAMETER;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Partition == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    RtlZeroMemory(Output, sizeof(*Output));

    if (!ReactVGuestWrite(Partition, Input->BaseGpa, Input->Data, Input->ByteCount))
    {
        Output->ResultCode = HvAccessGpaUnmapped;
        return HV_STATUS_SUCCESS;
    }

    Output->ResultCode = HvAccessGpaSuccess;

    return HV_STATUS_SUCCESS;
}

/* Resolves the child VP for Get/SetVpState */
static
PREACTV_VP
NTAPI
ReactVStateTargetVp(
    _In_ PREACTV_HYPERCALL Call,
    _In_ PHV_INPUT_ACCESS_VP_STATE Input)
{
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;

    Partition = ReactVPartitionFromCall(Call, Input->PartitionId);
    if ((Partition == NULL) || (Partition == ReactVRootPartition()))
        return NULL;

    if (Input->VpIndex >= REACTV_MAXIMUM_VPS)
        return NULL;

    Vp = &Partition->Vp[Input->VpIndex];
    if (!Vp->Created || (Vp->FpuState == 0))
        return NULL;

    return Vp;
}

/**
 * @brief
 * Locates the state buffer for Get/SetVpState.
 *
 * @remarks
 * With a variable header, its first qword is the PFN of the caller's buffer.
 * Otherwise the output page is used.
 */
static
_Must_inspect_result_
PVOID
NTAPI
ReactVVpStateArea(
    _In_ PREACTV_HYPERCALL Call,
    _In_ PHV_INPUT_ACCESS_VP_STATE Input)
{
    ULONG64 Header;
    ULONG64 Page;

    Header = (Call->Control >> HV_HYPERCALL_HEADER_SHIFT) & HV_HYPERCALL_HEADER_MASK;

    if (Header == 0)
        return ReactVHypercallOutput(Call, sizeof(*Input), AMD64_PAGE_SIZE);

    Page = *(ULONG64 *)((PUCHAR)Input + sizeof(*Input));

    return ReactVRootPointer(Page << AMD64_PAGE_SHIFT, AMD64_PAGE_SIZE);
}

/**
 * @brief
 * HvCallGetVpState for the XSAVE state type.
 *
 * @remarks
 * Returns the legacy XSAVE area followed by the XSTATE_BV qword. Only x87 and
 * SSE state are kept.
 */
static
ULONG64
NTAPI
ReactVGetVpState(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_ACCESS_VP_STATE Input;
    PUCHAR Output;
    PREACTV_VP Vp;

    /* Input includes the variable header */
    Input = ReactVHypercallInput(Call,
                                sizeof(*Input) +
                                    (((Call->Control >> HV_HYPERCALL_HEADER_SHIFT) &
                                      HV_HYPERCALL_HEADER_MASK) *
                                     HV_HYPERCALL_HEADER_UNIT));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Vp = ReactVStateTargetVp(Call, Input);
    if (Vp == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    if (Input->StateType != HvGetSetVpStateXsave)
    {
        ReactVLogWarning("Unsupported VP state type %u\n",
                         Input->StateType);
        return HV_STATUS_INVALID_PARAMETER;
    }

    /* Caller's read length is unknown, so zero the whole page */
    Output = ReactVVpStateArea(Call, Input);
    if (Output == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Output, AMD64_PAGE_SIZE);
    RtlCopyMemory(Output, (PVOID)(ULONG_PTR)Vp->FpuState, HV_XSAVE_LEGACY_SIZE);

    *(ULONG64 *)(Output + HV_XSAVE_LEGACY_SIZE) = HV_XSAVE_LEGACY_STATES;

    return HV_STATUS_SUCCESS;
}

/* HvCallSetVpState for the XSAVE state type */
static
ULONG64
NTAPI
ReactVSetVpState(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_ACCESS_VP_STATE Input;
    PREACTV_VP Vp;
    PUCHAR Area;
    ULONG64 Header;

    /* Variable header size in bytes */
    Header = ((Call->Control >> HV_HYPERCALL_HEADER_SHIFT) &
              HV_HYPERCALL_HEADER_MASK) * HV_HYPERCALL_HEADER_UNIT;

    Input = ReactVHypercallInput(Call,
                                sizeof(*Input) + Header + HV_XSAVE_LEGACY_SIZE);
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Vp = ReactVStateTargetVp(Call, Input);
    if (Vp == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    if (Input->StateType != HvGetSetVpStateXsave)
    {
        ReactVLogWarning("Unsupported VP state type %u for set\n",
                         Input->StateType);
        return HV_STATUS_INVALID_PARAMETER;
    }

    Area = ReactVVpStateArea(Call, Input);
    if (Area == NULL)
    {
        /* No buffer page: state is inline after the variable header */
        Area = (PUCHAR)Input + sizeof(*Input) + Header;
    }

    /* Legacy area only; XSTATE_BV is ignored */
    RtlCopyMemory((PVOID)(ULONG_PTR)Vp->FpuState, Area, HV_XSAVE_LEGACY_SIZE);

    return HV_STATUS_SUCCESS;
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVGuestHypercall(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *Status,
    _Out_ ULONG64 *RepsCompleted)
{
    *RepsCompleted = 0;

    switch (Call->Code)
    {
        case HvCallTranslateVirtualAddressEx:
            *Status = ReactVTranslateVirtualAddress(Call);
            return TRUE;

        case HvCallReadGpa:
            *Status = ReactVReadGpa(Call);
            return TRUE;

        case HvCallWriteGpa:
            *Status = ReactVWriteGpa(Call);
            return TRUE;

        case HvCallGetVpState:
            *Status = ReactVGetVpState(Call);
            return TRUE;

        case HvCallSetVpState:
            *Status = ReactVSetVpState(Call);
            return TRUE;

        default:
            break;
    }

    return FALSE;
}

/* EOF */
