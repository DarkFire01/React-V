/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hypervisor CPUID leaves, synthetic MSRs and hypercall dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"

/*
 * 0x40000000 is max leaf and vendor, 0x40000001 the interface signature,
 * 0x40000002 the version; leaves past the max read as zero.
 */

#define REACTV_CPUID_MAXIMUM_LEAF HV_CPUID_FUNCTION_HARDWARE_FEATURES

/* Implemented privileges; the root stack requires all of these */
#define REACTV_PARTITION_PRIVILEGES (HV_PARTITION_PRIVILEGE_ACCESS_TIME_REF_COUNT | \
                                     HV_PARTITION_PRIVILEGE_ACCESS_SYNIC_REGS | \
                                     HV_PARTITION_PRIVILEGE_ACCESS_SYNTHETIC_TIMER_REGS | \
                                     HV_PARTITION_PRIVILEGE_ACCESS_APIC_MSRS | \
                                     HV_PARTITION_PRIVILEGE_ACCESS_HYPERCALL_MSRS | \
                                     HV_PARTITION_PRIVILEGE_ACCESS_VP_INDEX)

/* Privileges offered when the SynIC is not ready */
#define REACTV_PARTITION_PRIVILEGES_BARE (HV_PARTITION_PRIVILEGE_ACCESS_HYPERCALL_MSRS | \
                                          HV_PARTITION_PRIVILEGE_ACCESS_VP_INDEX)

/*
 * Root partition rights. CPU_MANAGEMENT makes the root query the scheduler type
 * and run VPs on its own threads (root scheduler); without it the root expects
 * VP stop notifications this core does not implement.
 */
#define REACTV_PARTITION_RIGHTS (HV_PARTITION_PRIVILEGE_CREATE_PARTITIONS | \
                                 HV_PARTITION_PRIVILEGE_ACCESS_PARTITION_ID | \
                                 HV_PARTITION_PRIVILEGE_CPU_MANAGEMENT)

/* SVM runs real mode guests natively */
#define REACTV_HARDWARE_FEATURES HV_HARDWARE_FEATURE_UNRESTRICTED_GUEST

/* Hypercall page stub: vmmcall; ret */
#define REACTV_HYPERCALL_STUB_SIZE 4

static const UCHAR ReactVHypercallStub[REACTV_HYPERCALL_STUB_SIZE] =
{
    0x0F, 0x01, 0xD9, /* vmmcall */
    0xC3              /* ret */
};

/* FUNCTIONS ******************************************************************/

VOID
NTAPI
ReactVHvCpuid(
    _In_ ULONG Leaf,
    _In_ ULONG SubLeaf,
    _Inout_updates_(4) ULONG *Registers)
{
    UNREFERENCED_PARAMETER(SubLeaf);

    /* CPUID.1:ECX hypervisor present bit */
    if (Leaf == 1)
    {
        Registers[2] |= HV_CPUID_HYPERVISOR_PRESENT;
        return;
    }

    if ((Leaf < HV_CPUID_FUNCTION_VENDOR_AND_MAX_FUNCTION) ||
        (Leaf > HV_CPUID_FUNCTION_RANGE_END))
    {
        return;
    }

    Registers[0] = 0;
    Registers[1] = 0;
    Registers[2] = 0;
    Registers[3] = 0;

    if (Leaf > REACTV_CPUID_MAXIMUM_LEAF)
        return;

    switch (Leaf)
    {
        case HV_CPUID_FUNCTION_VENDOR_AND_MAX_FUNCTION:
            Registers[0] = REACTV_CPUID_MAXIMUM_LEAF;
            Registers[1] = REACTV_VENDOR_EBX;
            Registers[2] = REACTV_VENDOR_ECX;
            Registers[3] = REACTV_VENDOR_EDX;
            break;

        case HV_CPUID_FUNCTION_INTERFACE:
            Registers[0] = HV_INTERFACE_SIGNATURE;
            break;

        case HV_CPUID_FUNCTION_VERSION:
            Registers[0] = REACTV_VERSION_BUILD;
            Registers[1] = (REACTV_VERSION_MAJOR << 16) | REACTV_VERSION_MINOR;
            Registers[2] = REACTV_VERSION_SERVICE_PACK;
            Registers[3] = (REACTV_VERSION_SERVICE_BRANCH << 24) |
                           REACTV_VERSION_SERVICE_NUMBER;
            break;

        case HV_CPUID_FUNCTION_FEATURES:
            Registers[0] = ReactVCore.SynicReady ? REACTV_PARTITION_PRIVILEGES
                                                 : REACTV_PARTITION_PRIVILEGES_BARE;
            Registers[1] = REACTV_PARTITION_RIGHTS;
            break;

        case HV_CPUID_FUNCTION_ENLIGHTENMENT_INFORMATION:
            /* No enlightenments recommended */
            break;

        case HV_CPUID_FUNCTION_IMPLEMENTATION_LIMITS:
            /* Max VPs and max logical processors: 1 */
            Registers[0] = 1;
            Registers[1] = 1;
            break;

        case HV_CPUID_FUNCTION_HARDWARE_FEATURES:
            Registers[0] = REACTV_HARDWARE_FEATURES;
            break;

        default:
            break;
    }
}

static
VOID
NTAPI
ReactVWriteHypercallPage(
    _In_ ULONG64 Value)
{
    ReactVWriteHypercallStub(Value & HV_X64_MSR_HYPERCALL_PAGE_MASK);
}

/**
 * @brief
 * Writes a doorbell page stub that tags the call with its partition.
 *
 * @param[in] PageBase
 * Page owned by the partition.
 *
 * @param[in] PartitionId
 * Partition id loaded by the stub.
 *
 * @remarks
 * The exit does not identify which page the vmmcall came from, so the stub
 * loads the partition id into R9, which the hypercall ABI leaves unused.
 */
VOID
NTAPI
ReactVWriteDoorbellStub(
    _In_ ULONG64 PageBase,
    _In_ ULONG64 PartitionId)
{
    PUCHAR Page = (PUCHAR)(ULONG_PTR)PageBase;
    ULONG Index;

    for (Index = 0; Index < AMD64_PAGE_SIZE; Index++)
        Page[Index] = 0;

    /* mov r9, PartitionId */
    Page[0] = 0x49;
    Page[1] = 0xB9;

    for (Index = 0; Index < sizeof(PartitionId); Index++)
        Page[2 + Index] = (UCHAR)(PartitionId >> (Index * 8));

    for (Index = 0; Index < REACTV_HYPERCALL_STUB_SIZE; Index++)
        Page[10 + Index] = ReactVHypercallStub[Index];
}

/**
 * @brief
 * Writes the hypercall stub into a page.
 *
 * @param[in] PageBase
 * Physical address of the page. The root runs without NPT and the core maps
 * physical memory 1:1, so the GPA is used directly.
 */
VOID
NTAPI
ReactVWriteHypercallStub(
    _In_ ULONG64 PageBase)
{
    PUCHAR Page = (PUCHAR)(ULONG_PTR)PageBase;
    ULONG Index;

    for (Index = 0; Index < AMD64_PAGE_SIZE; Index++)
        Page[Index] = 0;

    for (Index = 0; Index < REACTV_HYPERCALL_STUB_SIZE; Index++)
        Page[Index] = ReactVHypercallStub[Index];
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVHvReadMsr(
    _In_ ULONG Index,
    _Out_ ULONG64 *Value)
{
    switch (Index)
    {
        case HV_X64_MSR_GUEST_OS_ID:
            *Value = ReactVCore.GuestOsId;
            return TRUE;

        case HV_X64_MSR_HYPERCALL:
            *Value = ReactVCore.HypercallMsr;
            return TRUE;

        case HV_X64_MSR_VP_INDEX:
            /* Single VP */
            *Value = 0;
            return TRUE;

        default:
            break;
    }

    if (ReactVCore.SynicReady && ReactVSynicReadMsr(Index, Value))
        return TRUE;

    if ((Index >= HV_X64_MSR_RANGE_BASE) && (Index <= HV_X64_MSR_RANGE_END))
        ReactVLogWarning("Unhandled synthetic MSR read %x\n", Index);

    /*
     * The MSRPM passes through every range it can describe, so anything here
     * does not exist. Fail it (#GP) instead of doing a rdmsr that faults the host.
     */
    UNREFERENCED_PARAMETER(Value);
    return FALSE;
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVHvWriteMsr(
    _In_ ULONG Index,
    _In_ ULONG64 Value)
{
    switch (Index)
    {
        case HV_X64_MSR_GUEST_OS_ID:
            ReactVCore.GuestOsId = Value;

            /* Clearing the guest OS id disables the hypercall page */
            if (Value == 0)
                ReactVCore.HypercallMsr = 0;

            return TRUE;

        case HV_X64_MSR_HYPERCALL:
            /* Writes are ignored until the guest OS id is set */
            if (ReactVCore.GuestOsId == 0)
                return TRUE;

            if ((Value & HV_X64_MSR_HYPERCALL_ENABLE) &&
                !(ReactVCore.HypercallMsr & HV_X64_MSR_HYPERCALL_ENABLE))
            {
                ReactVWriteHypercallPage(Value);
            }

            ReactVCore.HypercallMsr = Value;
            return TRUE;

        case HV_X64_MSR_VP_INDEX:
            /* Read only */
            return FALSE;

        default:
            break;
    }

    if (ReactVCore.SynicReady && ReactVSynicWriteMsr(Index, Value))
        return TRUE;

    if ((Index >= HV_X64_MSR_RANGE_BASE) && (Index <= HV_X64_MSR_RANGE_END))
        ReactVLogWarning("Unhandled synthetic MSR write %I64x to %x\n", Value, Index);

    UNREFERENCED_PARAMETER(Value);
    return FALSE;
}

/* Refused call codes already logged, so a retry loop logs once */
#define REACTV_REFUSED_CODES 0x200

static ULONG ReactVRefusedSeen[REACTV_REFUSED_CODES / 32];

static
VOID
NTAPI
ReactVHvReportRefused(
    _In_ ULONG64 Code,
    _In_ ULONG64 Control,
    _In_ ULONG64 InputGpa,
    _In_ ULONG64 OutputGpa)
{
    ULONG Index = (ULONG)(Code / 32);
    ULONG Bit = 1UL << (Code % 32);

    if (Code < REACTV_REFUSED_CODES)
    {
        if (ReactVRefusedSeen[Index] & Bit)
            return;

        ReactVRefusedSeen[Index] |= Bit;
    }

    ReactVLogTrace("Unimplemented hypercall %03x, control %016llx in %016llx out %016llx\n",
                   (ULONG)Code,
                   Control,
                   InputGpa,
                   OutputGpa);
}

/*
 * Returns the input buffer. Fast call input (RDX/R8, then XMM registers) was
 * already gathered into Call->Payload.
 */
_Must_inspect_result_
PVOID
NTAPI
ReactVHypercallInput(
    _In_ PREACTV_HYPERCALL Call,
    _In_ ULONG64 Length)
{
    if (!Call->Fast)
    {
        /* Child partition GPAs go through its own address space */
        if (Call->Caller != NULL)
            return ReactVGuestPointer(Call->Caller, Call->InputGpa, Length);

        if (Call->Virtual)
            return ReactVRootTranslate(Call->CallerCr3, Call->InputGpa, Length);

        return ReactVRootPointer(Call->InputGpa, Length);
    }

    if (Length > sizeof(Call->Payload))
        return NULL;

    return &Call->Payload[0];
}

_Must_inspect_result_
PVOID
NTAPI
ReactVHypercallOutput(
    _In_ PREACTV_HYPERCALL Call,
    _In_ ULONG64 InputLength,
    _In_ ULONG64 Length)
{
    ULONG64 Units;

    if (!Call->Fast)
    {
        if (Call->Caller != NULL)
            return ReactVGuestPointer(Call->Caller, Call->OutputGpa, Length);

        if (Call->Virtual)
            return ReactVRootTranslate(Call->CallerCr3, Call->OutputGpa, Length);

        return ReactVRootPointer(Call->OutputGpa, Length);
    }

    if (Call->State == NULL)
        return NULL;

    /*
     * Fast output follows the input in the XMM registers. Output with no input
     * would go to RDX/R8, which are not contiguous in the saved state. Unused,
     * so refused.
     */
    Units = (InputLength + 15) / 16;
    if (Units == 0)
        return NULL;

    Units--;

    if ((Units >= RTL_NUMBER_OF(Call->State->Xmm)) ||
        (Length > (sizeof(Call->State->Xmm) - (Units * 16))))
    {
        return NULL;
    }

    return &Call->State->Xmm[Units][0];
}

/* Scheduler type query; always the root scheduler (root threads run VPs) */
static
ULONG64
NTAPI
ReactVGetSystemInformation(
    _In_ PREACTV_HYPERCALL Call)
{
    PULONG64 Input;
    PULONG Output;

    Input = ReactVHypercallInput(Call, sizeof(ULONG64));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    if (*Input != HV_SYSTEM_INFORMATION_SCHEDULER_TYPE)
    {
        ReactVLogWarning("Unsupported system information class %I64u\n", *Input);
        return HV_STATUS_INVALID_PARAMETER;
    }

    Output = ReactVHypercallOutput(Call, sizeof(ULONG64), sizeof(ULONG));
    if (Output == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    *Output = HV_SCHEDULER_TYPE_ROOT;

    return HV_STATUS_SUCCESS;
}

/* Current logical processor index; always 0 */
static
ULONG64
NTAPI
ReactVLogicalProcessorIndex(
    _In_ PREACTV_HYPERCALL Call)
{
    PULONG64 Output;

    if (ReactVHypercallInput(Call, sizeof(ULONG64)) == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Output = ReactVHypercallOutput(Call, sizeof(ULONG64), 2 * sizeof(ULONG64));
    if (Output == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Output[0] = 0;
    Output[1] = 0;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Handles a hypercall from a child partition.
 *
 * @remarks
 * Only HvCallPostMessage and HvCallSignalEvent are allowed; everything else
 * returns HV_STATUS_ACCESS_DENIED.
 */
ULONG64
NTAPI
ReactVHvGuestHypercall(
    _In_ struct _REACTV_PARTITION *Partition,
    _In_ ULONG64 Control,
    _In_ ULONG64 InputGpa,
    _In_ ULONG64 OutputGpa)
{
    REACTV_HYPERCALL Call;
    ULONG64 RepsCompleted = 0;
    ULONG64 Status;

    RtlZeroMemory(&Call, sizeof(Call));

    Call.Control = Control;
    Call.Code = Control & HV_HYPERCALL_CODE_MASK;
    Call.RepCount = (Control & HV_HYPERCALL_REP_COUNT_MASK) >>
                    HV_HYPERCALL_REP_COUNT_SHIFT;
    Call.Fast = (Control & HV_HYPERCALL_FAST) != 0;
    Call.InputGpa = InputGpa;
    Call.OutputGpa = OutputGpa;
    Call.Caller = Partition;

    if (Call.Fast)
    {
        Call.Payload[0] = InputGpa;
        Call.Payload[1] = OutputGpa;
    }

    switch (Call.Code)
    {
        case HvCallPostMessage:
        case HvCallSignalEvent:
            break;

        default:
            return HV_STATUS_ACCESS_DENIED;
    }

    if (!ReactVPortHypercall(&Call, &Status, &RepsCompleted))
        return HV_STATUS_INVALID_HYPERCALL_CODE;

    if (Status != HV_STATUS_SUCCESS)
    {
        ReactVLogWarning("Guest hypercall %03I64x failed, status %I64u\n",
                         Call.Code,
                         Status);
    }

    return Status | (RepsCompleted << HV_HYPERCALL_REPS_COMPLETED_SHIFT);
}

ULONG64
NTAPI
ReactVHvHypercall(
    _In_ ULONG64 Control,
    _In_ ULONG64 InputGpa,
    _In_ ULONG64 OutputGpa,
    _In_ ULONG64 CallerCr3,
    _In_ PREACTV_GUEST_STATE State)
{
    REACTV_HYPERCALL Call;
    ULONG64 RepsCompleted = 0;
    ULONG64 Status;

    RtlZeroMemory(&Call, sizeof(Call));

    Call.Control = Control;
    Call.Code = Control & HV_HYPERCALL_CODE_MASK;
    Call.RepCount = (Control & HV_HYPERCALL_REP_COUNT_MASK) >>
                    HV_HYPERCALL_REP_COUNT_SHIFT;
    Call.Fast = (Control & HV_HYPERCALL_FAST) != 0;
    Call.Virtual = (Control & HV_HYPERCALL_VIRTUAL_ADDRESSES) != 0;
    Call.CallerCr3 = CallerCr3;
    Call.InputGpa = InputGpa;
    Call.OutputGpa = OutputGpa;
    Call.State = State;

    /* Fast call payload: RDX, R8, then the saved XMM registers */
    if (Call.Fast)
    {
        Call.Payload[0] = InputGpa;
        Call.Payload[1] = OutputGpa;

        if (State != NULL)
        {
            RtlCopyMemory(&Call.Payload[2],
                          &State->Xmm[0][0],
                          sizeof(State->Xmm));
        }
    }

    /* Hypercall page not enabled */
    if (!(ReactVCore.HypercallMsr & HV_X64_MSR_HYPERCALL_ENABLE))
        return HV_STATUS_INVALID_HYPERCALL_CODE;

    if (!Call.Fast)
    {
        /* Input and output GPAs must be 8 byte aligned */
        if ((InputGpa & 7) || (OutputGpa & 7))
            return HV_STATUS_INVALID_ALIGNMENT;
    }

    switch (Call.Code)
    {
        case HvCallNotifyLongSpinWait:
            /* Spin wait hint; a no-op is allowed by the TLFS */
            return HV_STATUS_SUCCESS;

        case HvCallGetSystemInformation:
            return ReactVGetSystemInformation(&Call);

        case ReactVCallLogicalProcessorIndex:
            return ReactVLogicalProcessorIndex(&Call);

        default:
            break;
    }

    /* Rep calls return reps completed so the caller can resume */
    if (ReactVPartitionHypercall(&Call, &Status, &RepsCompleted) ||
        ReactVGuestHypercall(&Call, &Status, &RepsCompleted) ||
        ReactVSynicHypercall(&Call, &Status, &RepsCompleted) ||
        ReactVPortHypercall(&Call, &Status, &RepsCompleted))
    {
        if (Status != HV_STATUS_SUCCESS)
        {
            ReactVLogWarning("Hypercall %03I64x failed, status %I64u, %I64u of %I64u reps, "
                             "control %I64x in %I64x\n",
                             Call.Code,
                             Status,
                             RepsCompleted,
                             Call.RepCount,
                             Call.Control,
                             Call.InputGpa);
        }

        return Status | (RepsCompleted << HV_HYPERCALL_REPS_COMPLETED_SHIFT);
    }

    /* Unimplemented; callers pick a fallback from the status, so it must match the TLFS */
    ReactVHvReportRefused(Call.Code, Control, InputGpa, OutputGpa);

    return HV_STATUS_INVALID_HYPERCALL_CODE;
}
