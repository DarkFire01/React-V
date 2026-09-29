/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Child partition VP dispatch and register access
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"

/*
 * A child VP runs on the calling root thread until it exits. No device model:
 * port and MSR accesses exit to the root with the exit reason and
 * EXITINFO1/EXITINFO2.
 */

/* Architectural reset state */
#define REACTV_RESET_CR0    0x0000000060000010ULL
#define REACTV_RESET_RFLAGS 0x0000000000000002ULL
#define REACTV_RESET_RIP    0x000000000000FFF0ULL
#define REACTV_RESET_CS_SELECTOR 0xF000
#define REACTV_RESET_CS_BASE 0x00000000FFFF0000ULL

/* Reset PAT. With NPT the guest PAT applies, and zero would make all guest memory UC */
#define REACTV_RESET_PAT 0x0007040600070406ULL

/* Max VMRUNs per dispatch, so exits the core handles itself cannot starve the root */
#define REACTV_DISPATCH_RUNS 4096


/* Present accessed R/W data, present accessed R/X code, busy TSS */
#define REACTV_ATTRIBUTES_DATA 0x0093
#define REACTV_ATTRIBUTES_CODE 0x009B
#define REACTV_ATTRIBUTES_TSS  0x008B

/* FUNCTIONS ******************************************************************/

static
PREACTV_VMCB
NTAPI
ReactVVmcbOf(
    _In_ PREACTV_VP Vp)
{
    /* Physical memory is identity mapped in the core */
    return (PREACTV_VMCB)(ULONG_PTR)Vp->Vmcb;
}

/**
 * @brief
 * Sets the VP's x87/SSE state to its reset values.
 *
 * @remarks
 * An all-zero area unmasks every x87 and SSE exception.
 */
static
VOID
NTAPI
ReactVResetFpuState(
    _In_ PREACTV_VP Vp)
{
    PREACTV_FPU_STATE State = (PREACTV_FPU_STATE)(ULONG_PTR)Vp->FpuState;

    if (State == NULL)
        return;

    RtlZeroMemory(State, sizeof(*State));

    State->ControlWord = REACTV_FPU_RESET_CONTROL;
    State->MxCsr = REACTV_FPU_RESET_MXCSR;
    State->MxCsrMask = REACTV_FPU_MXCSR_MASK;
}


VOID
NTAPI
ReactVChildBuildVmcb(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);

    RtlZeroMemory(Vmcb, sizeof(*Vmcb));

    /* IOPM and MSRPM are all ones, so every port and MSR access exits */
    Vmcb->Control.Intercept1 = REACTV_INTERCEPT_INTR |
                               REACTV_INTERCEPT_NMI |
                               REACTV_INTERCEPT_CPUID |
                               REACTV_INTERCEPT_HLT |
                               REACTV_INTERCEPT_IOIO |
                               REACTV_INTERCEPT_MSR |
                               REACTV_INTERCEPT_SHUTDOWN;
    Vmcb->Control.Intercept2 = REACTV_INTERCEPT_VMRUN | REACTV_INTERCEPT_VMMCALL;

    Vmcb->Control.IopmBasePa = ReactVCore.ChildIopm;
    Vmcb->Control.MsrpmBasePa = ReactVCore.ChildMsrpm;

    /* Per-partition NPT and ASID */
    Vmcb->Control.NestedPaging = REACTV_NESTED_PAGING_ENABLE;
    Vmcb->Control.NestedCr3 = Partition->Npt;
    Vmcb->Control.GuestAsid = Partition->Asid;
    Vmcb->Control.TlbControl = REACTV_TLB_FLUSH_GUEST;

    /* Reset state; the root overrides it with register writes before the first run */
    Vmcb->Save.Cr0 = REACTV_RESET_CR0;
    Vmcb->Save.Rflags = REACTV_RESET_RFLAGS;
    Vmcb->Save.Rip = REACTV_RESET_RIP;
    Vmcb->Save.Dr6 = REACTV_DR6_RESET;
    Vmcb->Save.Dr7 = REACTV_DR7_RESET;
    Vmcb->Save.GuestPat = REACTV_RESET_PAT;

    /* VMRUN fails if guest EFER.SVME is clear. Keep it set and hide it on reads */
    Vmcb->Save.Efer = AMD64_EFER_SVME;

    Vmcb->Save.Cs.Selector = REACTV_RESET_CS_SELECTOR;
    Vmcb->Save.Cs.Base = REACTV_RESET_CS_BASE;
    Vmcb->Save.Cs.Limit = 0xFFFF;
    Vmcb->Save.Cs.Attributes = REACTV_ATTRIBUTES_CODE;

    Vmcb->Save.Ds.Limit = 0xFFFF;
    Vmcb->Save.Ds.Attributes = REACTV_ATTRIBUTES_DATA;
    Vmcb->Save.Es = Vmcb->Save.Ds;
    Vmcb->Save.Fs = Vmcb->Save.Ds;
    Vmcb->Save.Gs = Vmcb->Save.Ds;
    Vmcb->Save.Ss = Vmcb->Save.Ds;

    Vmcb->Save.Gdtr.Limit = 0xFFFF;
    Vmcb->Save.Idtr.Limit = 0xFFFF;

    Vmcb->Save.Ldtr.Limit = 0xFFFF;
    Vmcb->Save.Ldtr.Attributes = 0x0082;

    Vmcb->Save.Tr.Limit = 0xFFFF;
    Vmcb->Save.Tr.Attributes = REACTV_ATTRIBUTES_TSS;

    ReactVResetFpuState(Vp);
    ReactVApicReset(Vp);
}

/* Forward declarations */
static
ULONG64
NTAPI
ReactVInterceptLength(
    _In_ PREACTV_VMCB Vmcb);

static
VOID
NTAPI
ReactVChildCpuid(
    _In_ ULONG Leaf,
    _In_ ULONG SubLeaf,
    _Out_writes_all_(4) PULONG Values);

static
ULONG64
NTAPI
ReactVPendingInterruption(
    _In_ PREACTV_VMCB Vmcb);

/* Segment conversion, defined with the register code below */
static
VOID
NTAPI
ReactVSegmentToRegister(
    _In_ PREACTV_VMCB_SEGMENT Segment,
    _Out_ PHV_REGISTER_VALUE Value);

static
VOID
NTAPI
ReactVRegisterToSegment(
    _In_ PHV_REGISTER_VALUE Value,
    _Out_ PREACTV_VMCB_SEGMENT Segment);

/**
 * @brief
 * Returns the FPU save area slot for an XMM or MMX register.
 *
 * @return
 * NULL if Name is not an XMM/MMX register or the VP has no FPU state.
 */
static
PVOID
NTAPI
ReactVVectorRegister(
    _In_ PREACTV_VP Vp,
    _In_ ULONG Name)
{
    PREACTV_FPU_STATE Fpu = (PREACTV_FPU_STATE)(ULONG_PTR)Vp->FpuState;

    if (Fpu == NULL)
        return NULL;

    if ((Name >= HvX64RegisterXmm0) && (Name <= HvX64RegisterXmm15))
        return Fpu->Vector[Name - HvX64RegisterXmm0];

    if ((Name >= HvX64RegisterFpMmx0) && (Name <= HvX64RegisterFpMmx7))
        return Fpu->Stack[Name - HvX64RegisterFpMmx0];

    return NULL;
}

/* Shared register page, or NULL */
static
PHV_VP_REGISTER_PAGE
NTAPI
ReactVRegisterPageOf(
    _In_ PREACTV_VP Vp)
{
    return (PHV_VP_REGISTER_PAGE)(ULONG_PTR)Vp->StatePage[HV_VP_STATE_PAGE_REGISTERS];
}

/**
 * @brief
 * Loads root writes from the shared register page before a run.
 *
 * @remarks
 * Only dirty groups are taken. The rest is stale from the last exit and would
 * undo an intercept the root already completed.
 */
static
VOID
NTAPI
ReactVTakeRegisterPage(
    _In_ PREACTV_VP Vp)
{
    PHV_VP_REGISTER_PAGE Page = ReactVRegisterPageOf(Vp);
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);
    ULONG Dirty;

    if (Page == NULL)
        return;

    Dirty = Page->Dirty;
    if (Dirty == 0)
        return;

    Page->Dirty = 0;

    if (Dirty & HV_REGISTER_PAGE_DIRTY_GP)
    {
        Vmcb->Save.Rax = Page->Gp[0];
        Vp->Registers.Rcx = Page->Gp[1];
        Vp->Registers.Rdx = Page->Gp[2];
        Vp->Registers.Rbx = Page->Gp[3];
        Vmcb->Save.Rsp = Page->Gp[4];
        Vp->Registers.Rbp = Page->Gp[5];
        Vp->Registers.Rsi = Page->Gp[6];
        Vp->Registers.Rdi = Page->Gp[7];
        Vp->Registers.R8 = Page->Gp[8];
        Vp->Registers.R9 = Page->Gp[9];
        Vp->Registers.R10 = Page->Gp[10];
        Vp->Registers.R11 = Page->Gp[11];
        Vp->Registers.R12 = Page->Gp[12];
        Vp->Registers.R13 = Page->Gp[13];
        Vp->Registers.R14 = Page->Gp[14];
        Vp->Registers.R15 = Page->Gp[15];
    }

    if (Dirty & HV_REGISTER_PAGE_DIRTY_RIP)
        Vmcb->Save.Rip = Page->Rip;

    if (Dirty & HV_REGISTER_PAGE_DIRTY_EXTENDED)
    {
        PREACTV_FPU_STATE Fpu = (PREACTV_FPU_STATE)(ULONG_PTR)Vp->FpuState;

        if (Fpu != NULL)
            RtlCopyMemory(Fpu->Vector, Page->Extended, sizeof(Page->Extended));
    }

    if (Dirty & HV_REGISTER_PAGE_DIRTY_RFLAGS)
        Vmcb->Save.Rflags = Page->Rflags;

    if (Dirty & HV_REGISTER_PAGE_DIRTY_SEGMENT)
    {
        ReactVRegisterToSegment((PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_ES],
                                &Vmcb->Save.Es);
        ReactVRegisterToSegment((PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_CS],
                                &Vmcb->Save.Cs);
        ReactVRegisterToSegment((PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_SS],
                                &Vmcb->Save.Ss);
        ReactVRegisterToSegment((PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_DS],
                                &Vmcb->Save.Ds);
        ReactVRegisterToSegment((PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_FS],
                                &Vmcb->Save.Fs);
        ReactVRegisterToSegment((PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_GS],
                                &Vmcb->Save.Gs);
    }

    Vmcb->Control.VmcbClean = 0;
}

/**
 * @brief
 * Publishes VP state to the shared register page after an exit.
 */
static
VOID
NTAPI
ReactVPublishRegisterPage(
    _In_ PREACTV_VP Vp)
{
    PHV_VP_REGISTER_PAGE Page = ReactVRegisterPageOf(Vp);
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);

    if (Page == NULL)
        return;

    Page->Gp[0] = Vmcb->Save.Rax;
    Page->Gp[1] = Vp->Registers.Rcx;
    Page->Gp[2] = Vp->Registers.Rdx;
    Page->Gp[3] = Vp->Registers.Rbx;
    Page->Gp[4] = Vmcb->Save.Rsp;
    Page->Gp[5] = Vp->Registers.Rbp;
    Page->Gp[6] = Vp->Registers.Rsi;
    Page->Gp[7] = Vp->Registers.Rdi;
    Page->Gp[8] = Vp->Registers.R8;
    Page->Gp[9] = Vp->Registers.R9;
    Page->Gp[10] = Vp->Registers.R10;
    Page->Gp[11] = Vp->Registers.R11;
    Page->Gp[12] = Vp->Registers.R12;
    Page->Gp[13] = Vp->Registers.R13;
    Page->Gp[14] = Vp->Registers.R14;
    Page->Gp[15] = Vp->Registers.R15;

    Page->Rip = Vmcb->Save.Rip;
    Page->Rflags = Vmcb->Save.Rflags;

    if (Vp->FpuState != 0)
    {
        PREACTV_FPU_STATE Fpu = (PREACTV_FPU_STATE)(ULONG_PTR)Vp->FpuState;

        RtlCopyMemory(Page->Extended, Fpu->Vector, sizeof(Page->Extended));
    }

    ReactVSegmentToRegister(&Vmcb->Save.Es,
                            (PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_ES]);
    ReactVSegmentToRegister(&Vmcb->Save.Cs,
                            (PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_CS]);
    ReactVSegmentToRegister(&Vmcb->Save.Ss,
                            (PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_SS]);
    ReactVSegmentToRegister(&Vmcb->Save.Ds,
                            (PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_DS]);
    ReactVSegmentToRegister(&Vmcb->Save.Fs,
                            (PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_FS]);
    ReactVSegmentToRegister(&Vmcb->Save.Gs,
                            (PHV_REGISTER_VALUE)&Page->Segment[HV_REGISTER_PAGE_SEGMENT_GS]);

    Page->Cr0 = Vmcb->Save.Cr0;
    Page->Cr3 = Vmcb->Save.Cr3;
    Page->Cr4 = Vmcb->Save.Cr4;
    /* TPR lives in the VMCB and is not used */
    Page->Cr8 = 0;

    /* Hide EFER.SVME */
    Page->Efer = Vmcb->Save.Efer & ~(ULONG64)AMD64_EFER_SVME;

    Page->Dr7 = Vmcb->Save.Dr7;

    /* Pending event, interrupt shadow and NMI mask */
    Page->PendingInterruption = ReactVPendingInterruption(Vmcb);

    Page->InterruptState = 0;

    if (Vmcb->Control.InterruptShadow & REACTV_SHADOW_INTERRUPT)
        Page->InterruptState |= HV_INTERRUPT_STATE_SHADOW;

    if (Vp->NmiMasked)
        Page->InterruptState |= HV_INTERRUPT_STATE_NMI_MASKED;

    Page->Version = HV_VP_REGISTER_PAGE_VERSION_1;
    Page->Vtl = 0;
    Page->IsValid = 1;
    Page->Dirty = 0;
}

/* Map an SVM exit code to an intercept reason */
static
ULONG64
NTAPI
ReactVInterceptReason(
    _In_ ULONG64 ExitCode)
{
    switch (ExitCode)
    {
        case REACTV_EXIT_HLT:
            return ReactVInterceptHalt;

        case REACTV_EXIT_IOIO:
            return ReactVInterceptIoPort;

        case REACTV_EXIT_NESTED_FAULT:
            return ReactVInterceptMemory;

        case REACTV_EXIT_CPUID:
            return ReactVInterceptCpuid;

        case REACTV_EXIT_MSR:
            return ReactVInterceptMsr;

        case REACTV_EXIT_SHUTDOWN:
            return ReactVInterceptShutdown;

        case REACTV_EXIT_VMMCALL:
            return ReactVInterceptHypercall;

        case REACTV_EXIT_VINTR:
            return ReactVInterceptDeliverable;

        case REACTV_EXIT_INTR:
        case REACTV_EXIT_NMI:
            return ReactVInterceptInterrupted;

        default:
            break;
    }

    return ReactVInterceptFailed;
}

/* Intercept message page, or NULL */
static
PHV_MESSAGE
NTAPI
ReactVMessagePageOf(
    _In_ PREACTV_VP Vp)
{
    return (PHV_MESSAGE)(ULONG_PTR)Vp->StatePage[HV_VP_STATE_PAGE_INTERCEPT_MESSAGE];
}

/* Common intercept message header */
static
VOID
NTAPI
ReactVFillMessageHeader(
    _In_ PREACTV_VP Vp,
    _In_ UCHAR AccessType,
    _Out_ PHV_X64_INTERCEPT_MESSAGE_HEADER Header)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);

    RtlZeroMemory(Header, sizeof(*Header));

    Header->VpIndex = Vp->Index;
    Header->InstructionLength = (UCHAR)ReactVInterceptLength(Vmcb);
    Header->InterceptAccessType = AccessType;

    /* CPL from CS.DPL */
    Header->ExecutionState = (USHORT)(Vmcb->Save.Cs.Attributes >> 5) & 3;

    if (Vmcb->Save.Cr0 & AMD64_CR0_PE)
        Header->ExecutionState |= HV_EXECUTION_STATE_CR0_PE;

    if (Vmcb->Save.Cr0 & AMD64_CR0_AM)
        Header->ExecutionState |= HV_EXECUTION_STATE_CR0_AM;

    if (Vmcb->Save.Efer & AMD64_EFER_LMA)
        Header->ExecutionState |= HV_EXECUTION_STATE_EFER_LMA;

    Header->CsSegment.Base = Vmcb->Save.Cs.Base;
    Header->CsSegment.Limit = Vmcb->Save.Cs.Limit;
    Header->CsSegment.Selector = Vmcb->Save.Cs.Selector;
    Header->CsSegment.Attributes = Vmcb->Save.Cs.Attributes;

    Header->Rip = Vmcb->Save.Rip;
    Header->Rflags = Vmcb->Save.Rflags;
}

/**
 * @brief
 * Writes the intercept message for the last exit.
 *
 * @return
 * FALSE if the exit has no message. The root then inspects the VP directly.
 */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVWriteInterceptMessage(
    _In_ PREACTV_VP Vp)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);
    PHV_MESSAGE Message = ReactVMessagePageOf(Vp);
    PHV_X64_MEMORY_INTERCEPT_MESSAGE Memory;
    PHV_X64_IO_PORT_INTERCEPT_MESSAGE Port;
    PHV_X64_CPUID_INTERCEPT_MESSAGE Cpuid;
    PHV_X64_MSR_INTERCEPT_MESSAGE Msr;
    PHV_X64_INTERRUPTION_DELIVERABLE_MESSAGE Deliverable;
    PHV_X64_HYPERCALL_INTERCEPT_MESSAGE Hyper;
    ULONG Values[4];
    ULONG64 Info;
    UCHAR Access;

    if (Message == NULL)
        return FALSE;

    RtlZeroMemory(Message, sizeof(*Message));

    switch (Vmcb->Control.ExitCode)
    {
        case REACTV_EXIT_NESTED_FAULT:
            Info = Vmcb->Control.ExitInfo1;

            if (Info & REACTV_NESTED_FAULT_FETCH)
                Access = HV_INTERCEPT_ACCESS_EXECUTE;
            else if (Info & REACTV_NESTED_FAULT_WRITE)
                Access = HV_INTERCEPT_ACCESS_WRITE;
            else
                Access = HV_INTERCEPT_ACCESS_READ;

            Memory = (PHV_X64_MEMORY_INTERCEPT_MESSAGE)Message->Payload;
            ReactVFillMessageHeader(Vp, Access, &Memory->Header);

            Memory->CacheType = REACTV_CACHE_WRITE_BACK;
            Memory->GuestPhysicalAddress = Vmcb->Control.ExitInfo2;

            /* GPA intercept, not unmapped GPA: the root populates guest pages on first touch */
            Message->MessageType = HvMessageTypeGpaIntercept;
            Message->PayloadSize = sizeof(*Memory);
            return TRUE;

        case REACTV_EXIT_IOIO:
            Info = Vmcb->Control.ExitInfo1;

            Access = (Info & REACTV_IOIO_TYPE_IN) ? HV_INTERCEPT_ACCESS_READ
                                                  : HV_INTERCEPT_ACCESS_WRITE;

            Port = (PHV_X64_IO_PORT_INTERCEPT_MESSAGE)Message->Payload;
            ReactVFillMessageHeader(Vp, Access, &Port->Header);

            Port->PortNumber = (USHORT)(Info >> REACTV_IOIO_PORT_SHIFT);

            /* One size bit per access width */
            Port->AccessInfo = (UCHAR)((Info & REACTV_IOIO_SIZE_MASK) >>
                                       REACTV_IOIO_SIZE_SHIFT);

            if (Info & REACTV_IOIO_STRING)
                Port->AccessInfo |= HV_PORT_ACCESS_STRING;

            if (Info & REACTV_IOIO_REPEAT)
                Port->AccessInfo |= HV_PORT_ACCESS_REP;

            Port->Rax = Vmcb->Save.Rax;
            Port->Rcx = Vp->Registers.Rcx;
            Port->Rsi = Vp->Registers.Rsi;
            Port->Rdi = Vp->Registers.Rdi;

            Port->DsSegment.Base = Vmcb->Save.Ds.Base;
            Port->DsSegment.Limit = Vmcb->Save.Ds.Limit;
            Port->DsSegment.Selector = Vmcb->Save.Ds.Selector;
            Port->DsSegment.Attributes = Vmcb->Save.Ds.Attributes;

            Port->EsSegment.Base = Vmcb->Save.Es.Base;
            Port->EsSegment.Limit = Vmcb->Save.Es.Limit;
            Port->EsSegment.Selector = Vmcb->Save.Es.Selector;
            Port->EsSegment.Attributes = Vmcb->Save.Es.Attributes;

            Message->MessageType = HvMessageTypeX64IoPortIntercept;
            Message->PayloadSize = sizeof(*Port);
            return TRUE;

        case REACTV_EXIT_CPUID:
            Cpuid = (PHV_X64_CPUID_INTERCEPT_MESSAGE)Message->Payload;
            ReactVFillMessageHeader(Vp, HV_INTERCEPT_ACCESS_EXECUTE, &Cpuid->Header);

            Cpuid->Rax = Vmcb->Save.Rax;
            Cpuid->Rcx = Vp->Registers.Rcx;
            Cpuid->Rdx = Vp->Registers.Rdx;
            Cpuid->Rbx = Vp->Registers.Rbx;

            ReactVChildCpuid((ULONG)Vmcb->Save.Rax,
                             (ULONG)Vp->Registers.Rcx,
                             Values);

            Cpuid->DefaultResultRax = Values[0];
            Cpuid->DefaultResultRbx = Values[1];
            Cpuid->DefaultResultRcx = Values[2];
            Cpuid->DefaultResultRdx = Values[3];

            Message->MessageType = HvMessageTypeX64CpuidIntercept;
            Message->PayloadSize = sizeof(*Cpuid);
            return TRUE;

        case REACTV_EXIT_MSR:
            /* EXITINFO1: 1 for WRMSR, 0 for RDMSR */
            Access = (Vmcb->Control.ExitInfo1 != 0) ? HV_INTERCEPT_ACCESS_WRITE
                                                    : HV_INTERCEPT_ACCESS_READ;

            Msr = (PHV_X64_MSR_INTERCEPT_MESSAGE)Message->Payload;
            ReactVFillMessageHeader(Vp, Access, &Msr->Header);

            Msr->MsrNumber = (ULONG)Vp->Registers.Rcx;

            /* WRMSR value in EDX:EAX */
            Msr->Rax = Vmcb->Save.Rax;
            Msr->Rdx = Vp->Registers.Rdx;

            Message->MessageType = HvMessageTypeX64MsrIntercept;
            Message->PayloadSize = sizeof(*Msr);
            return TRUE;

        case REACTV_EXIT_HLT:
            ReactVFillMessageHeader(Vp,
                                    HV_INTERCEPT_ACCESS_EXECUTE,
                                    (PHV_X64_INTERCEPT_MESSAGE_HEADER)Message->Payload);

            Message->MessageType = HvMessageTypeX64Halt;
            Message->PayloadSize = sizeof(HV_X64_INTERCEPT_MESSAGE_HEADER);
            return TRUE;

        case REACTV_EXIT_VMMCALL:
            Hyper = (PHV_X64_HYPERCALL_INTERCEPT_MESSAGE)Message->Payload;
            ReactVFillMessageHeader(Vp, HV_INTERCEPT_ACCESS_EXECUTE, &Hyper->Header);

            /* Hypercall ABI registers only */
            Hyper->Rax = Vmcb->Save.Rax;
            Hyper->Rbx = Vp->Registers.Rbx;
            Hyper->Rcx = Vp->Registers.Rcx;
            Hyper->Rdx = Vp->Registers.Rdx;
            Hyper->R8 = Vp->Registers.R8;
            Hyper->Rsi = Vp->Registers.Rsi;
            Hyper->Rdi = Vp->Registers.Rdi;

            if (Vp->FpuState != 0)
            {
                PREACTV_FPU_STATE Fpu = (PREACTV_FPU_STATE)(ULONG_PTR)Vp->FpuState;

                RtlCopyMemory(Hyper->Xmm, Fpu->Vector, sizeof(Hyper->Xmm));
            }

            Message->MessageType = HvMessageTypeHypercallIntercept;
            Message->PayloadSize = sizeof(*Hyper);
            return TRUE;

        case REACTV_EXIT_SHUTDOWN:
            /* Unrecoverable, the root decides what to do */
            ReactVFillMessageHeader(Vp,
                                    HV_INTERCEPT_ACCESS_EXECUTE,
                                    (PHV_X64_INTERCEPT_MESSAGE_HEADER)Message->Payload);

            Message->MessageType = HvMessageTypeUnrecoverableException;
            Message->PayloadSize = sizeof(HV_X64_INTERCEPT_MESSAGE_HEADER);
            return TRUE;

        case REACTV_EXIT_VINTR:
            Deliverable = (PHV_X64_INTERRUPTION_DELIVERABLE_MESSAGE)Message->Payload;
            ReactVFillMessageHeader(Vp,
                                    HV_INTERCEPT_ACCESS_EXECUTE,
                                    &Deliverable->Header);

            Deliverable->DeliverableType = HV_DELIVERABLE_TYPE_INTERRUPT;

            Message->MessageType = HvMessageTypeX64InterruptionDeliverable;
            Message->PayloadSize = sizeof(*Deliverable);
            return TRUE;

        default:
            break;
    }

    return FALSE;
}

/**
 * @brief
 * Returns the event still pending in EVENTINJ, in HV pending interruption format.
 *
 * @remarks
 * EVENTINJ is cleared once the event is delivered, so anything left is still pending.
 */
static
ULONG64
NTAPI
ReactVPendingInterruption(
    _In_ PREACTV_VMCB Vmcb)
{
    ULONG64 Inject = Vmcb->Control.EventInject;
    ULONG64 Pending;

    if (!(Inject & REACTV_EVENT_VALID))
        return 0;

    Pending = HV_PENDING_INTERRUPTION_PENDING;
    Pending |= (ULONG64)((Inject >> REACTV_EVENT_TYPE_SHIFT) & REACTV_EVENT_TYPE_MASK) <<
               HV_PENDING_INTERRUPTION_TYPE_SHIFT;
    Pending |= (Inject & REACTV_EVENT_VECTOR_MASK) << HV_PENDING_INTERRUPTION_VECTOR_SHIFT;

    if (Inject & REACTV_EVENT_ERROR_VALID)
    {
        Pending |= HV_PENDING_INTERRUPTION_ERROR_CODE_VALID;
        Pending |= ((Inject >> REACTV_EVENT_ERROR_SHIFT) & 0xFFFFFFFF) <<
                   HV_PENDING_INTERRUPTION_ERROR_CODE_SHIFT;
    }

    return Pending;
}

/* Inject the root's pending event, or cancel one not yet delivered */
static
VOID
NTAPI
ReactVInject(
    _In_ PREACTV_VMCB Vmcb,
    _In_ ULONG64 Pending)
{
    ULONG64 Inject;
    ULONG Vector, Type;

    if (!(Pending & HV_PENDING_INTERRUPTION_PENDING))
    {
        Vmcb->Control.EventInject = 0;
        return;
    }

    Vector = (ULONG)((Pending >> HV_PENDING_INTERRUPTION_VECTOR_SHIFT) &
                     HV_PENDING_INTERRUPTION_VECTOR_MASK);
    Type = (ULONG)((Pending >> HV_PENDING_INTERRUPTION_TYPE_SHIFT) &
                   HV_PENDING_INTERRUPTION_TYPE_MASK);

    Inject = (Vector & REACTV_EVENT_VECTOR_MASK) |
             ((ULONG64)Type << REACTV_EVENT_TYPE_SHIFT) |
             REACTV_EVENT_VALID;

    if (Pending & HV_PENDING_INTERRUPTION_ERROR_CODE_VALID)
    {
        Inject |= REACTV_EVENT_ERROR_VALID;
        Inject |= ((Pending >> HV_PENDING_INTERRUPTION_ERROR_CODE_SHIFT) & 0xFFFFFFFF) <<
                  REACTV_EVENT_ERROR_SHIFT;
    }

    Vmcb->Control.EventInject = Inject;
}

/* TRUE if RFLAGS.IF is set and there is no interrupt shadow */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVChildInterruptible(
    _In_ PREACTV_VMCB Vmcb)
{
    if (!(Vmcb->Save.Rflags & AMD64_RFLAGS_IF))
        return FALSE;

    if (Vmcb->Control.InterruptShadow & REACTV_SHADOW_INTERRUPT)
        return FALSE;

    return TRUE;
}

/**
 * @brief
 * Arms or disarms the interrupt window exit.
 *
 * @remarks
 * Uses V_IRQ with the VINTR intercept: the VP exits as soon as it could take
 * the virtual interrupt, which is never actually delivered.
 */
static
VOID
NTAPI
ReactVWatchForDeliverability(
    _In_ PREACTV_VP Vp)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);

    Vmcb->Control.VmcbClean = 0;

    /* Root requested notification, or a SynIC/APIC interrupt is pending */
    if ((Vp->Deliverability & HV_DELIVERABILITY_INTERRUPT) ||
        (Vp->Synic.Pending != 0) ||
        ReactVApicWaiting(Vp))
    {
        Vmcb->Control.Intercept1 |= REACTV_INTERCEPT_VINTR;
        Vmcb->Control.VirtualInterrupt |= REACTV_VINTR_IRQ | REACTV_VINTR_IGNORE_TPR |
                                          (15ULL << REACTV_VINTR_PRIORITY_SHIFT);
        return;
    }

    Vmcb->Control.Intercept1 &= ~(ULONG)REACTV_INTERCEPT_VINTR;
    Vmcb->Control.VirtualInterrupt &= ~(REACTV_VINTR_IRQ | REACTV_VINTR_IGNORE_TPR |
                                        (15ULL << REACTV_VINTR_PRIORITY_SHIFT));
}

/* Inject the next pending SynIC vector, else the next pending APIC vector */
static
VOID
NTAPI
ReactVGiveWaitingMessage(
    _In_ PREACTV_VP Vp)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);
    ULONG Vector = ReactVSynicTakePending(Vp);

    if (Vector == 0)
        Vector = ReactVApicTakePending(Vp);

    if (Vector == 0)
        return;

    Vmcb->Control.EventInject = Vector |
                                ((ULONG64)REACTV_EVENT_TYPE_INTERRUPT <<
                                 REACTV_EVENT_TYPE_SHIFT) |
                                REACTV_EVENT_VALID;
    Vmcb->Control.VmcbClean = 0;
}

/**
 * @brief
 * Handles a SynIC or APIC MSR access in the core and advances RIP.
 *
 * @return
 * FALSE if the core does not handle the MSR. The exit then goes to the root.
 */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVChildSyntheticMsr(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);
    ULONG Index = (ULONG)Vp->Registers.Rcx;
    ULONG64 Value = 0;
    ULONG64 Length;

    /* EXITINFO1: 1 for WRMSR, 0 for RDMSR */
    if (Vmcb->Control.ExitInfo1 != 0)
    {
        Value = ((ULONG64)(ULONG)Vp->Registers.Rdx << 32) | (ULONG)Vmcb->Save.Rax;

        if (!ReactVSynicChildMsr(Partition, Vp, Index, TRUE, &Value) &&
            !ReactVApicMsr(Partition, Vp, Index, TRUE, &Value))
        {
            return FALSE;
        }
    }
    else
    {
        if (!ReactVSynicChildMsr(Partition, Vp, Index, FALSE, &Value) &&
            !ReactVApicMsr(Partition, Vp, Index, FALSE, &Value))
        {
            return FALSE;
        }

        Vmcb->Save.Rax = (ULONG)Value;
        Vp->Registers.Rdx = (ULONG)(Value >> 32);
    }

    Length = ReactVInterceptLength(Vmcb);
    if (Length == 0)
        return FALSE;

    Vmcb->Save.Rip += Length;

    return TRUE;
}

/* TRUE if the root intercepts this CPUID leaf */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVCpuidIsIntercepted(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG Leaf)
{
    ULONG Index;

    if (Partition->Intercepts.EveryCpuid)
        return TRUE;

    for (Index = 0; Index < Partition->Intercepts.CpuidCount; Index++)
    {
        if (Partition->Intercepts.CpuidLeaf[Index] == Leaf)
            return TRUE;
    }

    return FALSE;
}

/**
 * @brief
 * Handles exits the core can complete without the root.
 *
 * @return
 * TRUE if the exit was handled and the VP can be resumed.
 */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVChildAnswered(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);
    ULONG Values[4];
    ULONG64 Length;

    /* Interrupt window the core requested: inject the waiting interrupt */
    if (Vmcb->Control.ExitCode == REACTV_EXIT_VINTR)
    {
        if (Vp->Deliverability & HV_DELIVERABILITY_INTERRUPT)
            return FALSE;

        ReactVGiveWaitingMessage(Vp);
        ReactVWatchForDeliverability(Vp);

        return TRUE;
    }

    /* Hypercalls the root does not intercept complete here */
    if (Vmcb->Control.ExitCode == REACTV_EXIT_VMMCALL)
    {
        ULONG64 Step;

        if (Partition->Intercepts.EveryHypercall)
            return FALSE;

        Step = ReactVInterceptLength(Vmcb);
        if (Step == 0)
            return FALSE;

        Vmcb->Save.Rax = ReactVHvGuestHypercall(Partition,
                                                Vp->Registers.Rcx,
                                                Vp->Registers.Rdx,
                                                Vp->Registers.R8);
        Vmcb->Save.Rip += Step;

        return TRUE;
    }

    /* SynIC and APIC MSRs are handled by the core */
    if (Vmcb->Control.ExitCode == REACTV_EXIT_MSR)
        return ReactVChildSyntheticMsr(Partition, Vp);

    if (Vmcb->Control.ExitCode != REACTV_EXIT_CPUID)
        return FALSE;

    if (ReactVCpuidIsIntercepted(Partition, (ULONG)Vmcb->Save.Rax))
        return FALSE;

    ReactVChildCpuid((ULONG)Vmcb->Save.Rax, (ULONG)Vp->Registers.Rcx, Values);

    Vmcb->Save.Rax = Values[0];
    Vp->Registers.Rbx = Values[1];
    Vp->Registers.Rcx = Values[2];
    Vp->Registers.Rdx = Values[3];

    Length = ReactVInterceptLength(Vmcb);
    if (Length == 0)
        return FALSE;

    Vmcb->Save.Rip += Length;

    return TRUE;
}

ULONG64
NTAPI
ReactVChildDispatch(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _Out_ ULONG64 *ExitReason,
    _Out_ PBOOLEAN Message)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);
    /* Current physical processor */
    PREACTV_PROCESSOR Processor = ReactVCurrentProcessor();
    ULONG Runs;

    *ExitReason = ReactVInterceptNone;
    *Message = FALSE;

    Vp->Started = TRUE;

    /* Apply root writes from the shared register page */
    ReactVTakeRegisterPage(Vp);

    /* Save host FPU state, restored at the end of the dispatch */
    ReactVSvmSaveFpu((PVOID)(ULONG_PTR)Processor->HostFpuState);

    for (Runs = 0; Runs < REACTV_DISPATCH_RUNS; Runs++)
    {
        /* NPT may have changed since the last run, so flush the guest ASID every time */
        Vmcb->Control.NestedCr3 = Partition->Npt;
        Vmcb->Control.TlbControl = REACTV_TLB_FLUSH_GUEST;
        Vmcb->Control.VmcbClean = 0;

        /* Expire synthetic timers */
        ReactVSynicTimers(Partition, Vp);

        /* Write the hypercall page once the guest has enabled it */
        if (Partition->HypercallStubWanted)
            ReactVSynicWriteCallPage(Partition);

        /* Inject pending SynIC/APIC interrupts, or wait for a window */
        if (((Vp->Synic.Pending != 0) || ReactVApicWaiting(Vp)) &&
            (Vmcb->Control.EventInject == 0))
        {
            if (ReactVChildInterruptible(Vmcb))
                ReactVGiveWaitingMessage(Vp);
            else
                ReactVWatchForDeliverability(Vp);
        }

        /* Guest FPU state is live only across VMRUN; the core uses those registers */
        ReactVSvmRestoreFpu((PVOID)(ULONG_PTR)Vp->FpuState);

        ReactVSvmRunChild(Vp->Vmcb, &Vp->Registers, Processor->HostVmcb);

        ReactVSvmSaveFpu((PVOID)(ULONG_PTR)Vp->FpuState);

        /*
         * An exit during event delivery leaves the event in EXITINTINFO; reinject
         * it. This happens on the first touch of an unbacked guest IDT page.
         */
        if (Vmcb->Control.ExitIntInfo & REACTV_EVENT_VALID)
        {
            Vmcb->Control.EventInject = Vmcb->Control.ExitIntInfo;
            Vmcb->Control.VmcbClean = 0;
        }

        /* VMEXIT_INVALID is a core bug the root cannot fix */
        if (Vmcb->Control.ExitCode == REACTV_EXIT_INVALID)
        {
            ReactVLogError("Partition %I64u VP %lu VMEXIT_INVALID, rip %I64x cr0 %I64x\n",
                           Partition->Id,
                           Vp->Index,
                           Vmcb->Save.Rip,
                           Vmcb->Save.Cr0);

            ReactVSvmRestoreFpu((PVOID)(ULONG_PTR)Processor->HostFpuState);
            ReactVPublishRegisterPage(Vp);

            *ExitReason = ReactVInterceptFailed;
            return HV_STATUS_OPERATION_FAILED;
        }

        /* Resume on exits the core handled, bounded by REACTV_DISPATCH_RUNS */
        if (ReactVChildAnswered(Partition, Vp))
            continue;

        /* Root interrupt window notifications are one shot; the root re-arms them */
        if (Vmcb->Control.ExitCode == REACTV_EXIT_VINTR)
        {
            Vp->Deliverability &= ~(ULONG64)HV_DELIVERABILITY_INTERRUPT;
            ReactVWatchForDeliverability(Vp);
        }

        *ExitReason = ReactVInterceptReason(Vmcb->Control.ExitCode);
        *Message = ReactVWriteInterceptMessage(Vp);

        /*
         * Log exits with no message; the root treats them as no exit.
         * INTR and NMI are expected host preemption.
         */
        if (!*Message &&
            (Vmcb->Control.ExitCode != REACTV_EXIT_INTR) &&
            (Vmcb->Control.ExitCode != REACTV_EXIT_NMI))
        {
            ReactVLogWarning("Partition %I64u VP %lu exit %I64x, "
                             "rip %I64x, no intercept message\n",
                             Partition->Id,
                             Vp->Index,
                             Vmcb->Control.ExitCode,
                             Vmcb->Save.Rip);
        }

        break;
    }

    ReactVSvmRestoreFpu((PVOID)(ULONG_PTR)Processor->HostFpuState);

    ReactVPublishRegisterPage(Vp);

    /* RIP stays on the intercepted instruction; the root advances it after completing the access */

    return HV_STATUS_SUCCESS;
}

/*
 * Fixed lengths for exits whose instruction has one encoding. Emulated CPUs
 * often provide neither NRIP nor the fetched bytes.
 */
static
ULONG64
NTAPI
ReactVKnownLength(
    _In_ ULONG64 ExitCode)
{
    switch (ExitCode)
    {
        case REACTV_EXIT_HLT:
            return 1;

        case REACTV_EXIT_CPUID:
        case REACTV_EXIT_MSR:
            return 2;

        case REACTV_EXIT_VMMCALL:
            return 3;

        default:
            break;
    }

    return 0;
}

/*
 * Length of the intercepted instruction. IOIO has the next RIP in EXITINFO2,
 * other exits use NRIP when supported. Returns 0 if unknown.
 */
static
ULONG64
NTAPI
ReactVInterceptLength(
    _In_ PREACTV_VMCB Vmcb)
{
    ULONG64 Next = Vmcb->Control.NextRip;

    if (Vmcb->Control.ExitCode == REACTV_EXIT_IOIO)
        Next = Vmcb->Control.ExitInfo2;

    if ((Next > Vmcb->Save.Rip) && ((Next - Vmcb->Save.Rip) <= 15))
        return Next - Vmcb->Save.Rip;

    if (Vmcb->Control.FetchedInstructionLength != 0)
        return Vmcb->Control.FetchedInstructionLength;

    return ReactVKnownLength(Vmcb->Control.ExitCode);
}

/**
 * @brief
 * Default CPUID result for a child, also offered to the root in the intercept message.
 *
 * @remarks
 * Reports the hypervisor leaves and hides SVM, since nested virtualization is not supported.
 */
static
VOID
NTAPI
ReactVChildCpuid(
    _In_ ULONG Leaf,
    _In_ ULONG SubLeaf,
    _Out_writes_all_(4) PULONG Values)
{
    int Result[4];

    __cpuidex(Result, (int)Leaf, (int)SubLeaf);

    Values[0] = (ULONG)Result[0];
    Values[1] = (ULONG)Result[1];
    Values[2] = (ULONG)Result[2];
    Values[3] = (ULONG)Result[3];

    if (Leaf == AMD64_CPUID_EXTENDED_FEATURES)
    {
        Values[2] &= ~(ULONG)AMD64_EXT_FEATURE_SVM;
        return;
    }

    /* Hypervisor leaves are answered by the core. Children get no root privileges */
    if ((Leaf < HV_CPUID_FUNCTION_VENDOR_AND_MAX_FUNCTION) ||
        (Leaf > HV_CPUID_FUNCTION_RANGE_END))
    {
        if (Leaf == 1)
            Values[2] |= HV_CPUID_HYPERVISOR_PRESENT;

        return;
    }

    Values[0] = 0;
    Values[1] = 0;
    Values[2] = 0;
    Values[3] = 0;

    switch (Leaf)
    {
        case HV_CPUID_FUNCTION_VENDOR_AND_MAX_FUNCTION:
            Values[0] = HV_CPUID_FUNCTION_VERSION;
            Values[1] = REACTV_VENDOR_EBX;
            Values[2] = REACTV_VENDOR_ECX;
            Values[3] = REACTV_VENDOR_EDX;
            break;

        case HV_CPUID_FUNCTION_INTERFACE:
            Values[0] = HV_INTERFACE_SIGNATURE;
            break;

        case HV_CPUID_FUNCTION_VERSION:
            Values[0] = REACTV_VERSION_BUILD;
            Values[1] = (REACTV_VERSION_MAJOR << 16) | REACTV_VERSION_MINOR;
            Values[2] = REACTV_VERSION_SERVICE_PACK;
            Values[3] = (REACTV_VERSION_SERVICE_BRANCH << 24) |
                        REACTV_VERSION_SERVICE_NUMBER;
            break;

        case HV_CPUID_FUNCTION_FEATURES:
            /* Only what the core implements for a child */
            Values[0] = HV_PARTITION_PRIVILEGE_ACCESS_TIME_REF_COUNT |
                        HV_PARTITION_PRIVILEGE_ACCESS_SYNIC_REGS |
                        HV_PARTITION_PRIVILEGE_ACCESS_SYNTHETIC_TIMER_REGS |
                        HV_PARTITION_PRIVILEGE_ACCESS_APIC_MSRS |
                        HV_PARTITION_PRIVILEGE_ACCESS_HYPERCALL_MSRS |
                        HV_PARTITION_PRIVILEGE_ACCESS_VP_INDEX;
            break;

        case HV_CPUID_FUNCTION_ENLIGHTENMENT_INFORMATION:
            /* Recommend APIC MSRs over MMIO, which would need instruction decoding */
            Values[0] = HV_ENLIGHTENMENT_USE_APIC_MSRS;
            Values[1] = HV_ENLIGHTENMENT_NEVER_NOTIFY;
            break;

        default:
            break;
    }
}

/* REGISTERS ******************************************************************/

static
VOID
NTAPI
ReactVSegmentToRegister(
    _In_ PREACTV_VMCB_SEGMENT Segment,
    _Out_ PHV_REGISTER_VALUE Value)
{
    PHV_X64_SEGMENT_REGISTER Register = (PHV_X64_SEGMENT_REGISTER)Value;

    Register->Base = Segment->Base;
    Register->Limit = Segment->Limit;
    Register->Selector = Segment->Selector;
    Register->Attributes = Segment->Attributes;
}

static
VOID
NTAPI
ReactVRegisterToSegment(
    _In_ PHV_REGISTER_VALUE Value,
    _Out_ PREACTV_VMCB_SEGMENT Segment)
{
    PHV_X64_SEGMENT_REGISTER Register = (PHV_X64_SEGMENT_REGISTER)Value;

    Segment->Base = Register->Base;
    Segment->Limit = Register->Limit;
    Segment->Selector = Register->Selector;
    Segment->Attributes = Register->Attributes;
}

/* GDTR/IDTR: base and limit only */
static
VOID
NTAPI
ReactVTableToRegister(
    _In_ PREACTV_VMCB_SEGMENT Segment,
    _Out_ PHV_REGISTER_VALUE Value)
{
    PHV_X64_TABLE_REGISTER Register = (PHV_X64_TABLE_REGISTER)Value;

    RtlZeroMemory(Register, sizeof(*Register));
    Register->Limit = (USHORT)Segment->Limit;
    Register->Base = Segment->Base;
}

static
VOID
NTAPI
ReactVRegisterToTable(
    _In_ PHV_REGISTER_VALUE Value,
    _Out_ PREACTV_VMCB_SEGMENT Segment)
{
    PHV_X64_TABLE_REGISTER Register = (PHV_X64_TABLE_REGISTER)Value;

    Segment->Limit = Register->Limit;
    Segment->Base = Register->Base;
}

ULONG64
NTAPI
ReactVChildReadRegister(
    _In_ PREACTV_VP Vp,
    _In_ ULONG Name,
    _Out_ PHV_REGISTER_VALUE Value)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);
    PREACTV_FPU_STATE Fpu = (PREACTV_FPU_STATE)(ULONG_PTR)Vp->FpuState;
    PVOID Vector;

    Value->Low = 0;
    Value->High = 0;

    /* XMM and MMX registers come from the FPU save area */
    Vector = ReactVVectorRegister(Vp, Name);
    if (Vector != NULL)
    {
        RtlCopyMemory(Value, Vector, sizeof(*Value));
        return HV_STATUS_SUCCESS;
    }

    switch (Name)
    {
        case HvX64RegisterRax: Value->Low = Vmcb->Save.Rax; break;
        case HvX64RegisterRcx: Value->Low = Vp->Registers.Rcx; break;
        case HvX64RegisterRdx: Value->Low = Vp->Registers.Rdx; break;
        case HvX64RegisterRbx: Value->Low = Vp->Registers.Rbx; break;
        case HvX64RegisterRsp: Value->Low = Vmcb->Save.Rsp; break;
        case HvX64RegisterRbp: Value->Low = Vp->Registers.Rbp; break;
        case HvX64RegisterRsi: Value->Low = Vp->Registers.Rsi; break;
        case HvX64RegisterRdi: Value->Low = Vp->Registers.Rdi; break;
        case HvX64RegisterR8:  Value->Low = Vp->Registers.R8; break;
        case HvX64RegisterR9:  Value->Low = Vp->Registers.R9; break;
        case HvX64RegisterR10: Value->Low = Vp->Registers.R10; break;
        case HvX64RegisterR11: Value->Low = Vp->Registers.R11; break;
        case HvX64RegisterR12: Value->Low = Vp->Registers.R12; break;
        case HvX64RegisterR13: Value->Low = Vp->Registers.R13; break;
        case HvX64RegisterR14: Value->Low = Vp->Registers.R14; break;
        case HvX64RegisterR15: Value->Low = Vp->Registers.R15; break;
        case HvX64RegisterRip: Value->Low = Vmcb->Save.Rip; break;
        case HvX64RegisterRflags: Value->Low = Vmcb->Save.Rflags; break;

        case HvX64RegisterCr0:
        case HvX64RegisterCr0Paged: Value->Low = Vmcb->Save.Cr0; break;
        case HvX64RegisterCr2: Value->Low = Vmcb->Save.Cr2; break;
        case HvX64RegisterCr3:
        case HvX64RegisterCr3Paged: Value->Low = Vmcb->Save.Cr3; break;
        case HvX64RegisterCr4:
        case HvX64RegisterCr4Paged: Value->Low = Vmcb->Save.Cr4; break;
        case HvX64RegisterCr8Paged: Value->Low = 0; break;
        case HvX64RegisterEfer:
            /* Hide EFER.SVME */
            Value->Low = Vmcb->Save.Efer & ~AMD64_EFER_SVME;
            break;

        case HvX64RegisterPat: Value->Low = Vmcb->Save.GuestPat; break;

        case HvX64RegisterEs: ReactVSegmentToRegister(&Vmcb->Save.Es, Value); break;
        case HvX64RegisterCs: ReactVSegmentToRegister(&Vmcb->Save.Cs, Value); break;
        case HvX64RegisterSs: ReactVSegmentToRegister(&Vmcb->Save.Ss, Value); break;
        case HvX64RegisterDs: ReactVSegmentToRegister(&Vmcb->Save.Ds, Value); break;
        case HvX64RegisterFs: ReactVSegmentToRegister(&Vmcb->Save.Fs, Value); break;
        case HvX64RegisterGs: ReactVSegmentToRegister(&Vmcb->Save.Gs, Value); break;
        case HvX64RegisterLdtr: ReactVSegmentToRegister(&Vmcb->Save.Ldtr, Value); break;
        case HvX64RegisterTr: ReactVSegmentToRegister(&Vmcb->Save.Tr, Value); break;

        case HvX64RegisterGdtr: ReactVTableToRegister(&Vmcb->Save.Gdtr, Value); break;
        case HvX64RegisterIdtr: ReactVTableToRegister(&Vmcb->Save.Idtr, Value); break;

        case HvX64RegisterKernelGsBase: Value->Low = Vmcb->Save.KernelGsBase; break;
        case HvX64RegisterStar: Value->Low = Vmcb->Save.Star; break;
        case HvX64RegisterLstar: Value->Low = Vmcb->Save.LStar; break;
        case HvX64RegisterCstar: Value->Low = Vmcb->Save.CStar; break;
        case HvX64RegisterSfmask: Value->Low = Vmcb->Save.SfMask; break;
        case HvX64RegisterSysenterCs: Value->Low = Vmcb->Save.SysenterCs; break;
        case HvX64RegisterSysenterEsp: Value->Low = Vmcb->Save.SysenterEsp; break;
        case HvX64RegisterSysenterEip: Value->Low = Vmcb->Save.SysenterEip; break;

        case HvX64RegisterFpControlStatus:
            if (Fpu != NULL)
            {
                Value->Low = (ULONG64)Fpu->ControlWord |
                             ((ULONG64)Fpu->StatusWord << 16) |
                             ((ULONG64)Fpu->TagWord << 32) |
                             ((ULONG64)Fpu->Opcode << 48);
                Value->High = Fpu->InstructionPointer;
            }

            break;

        case HvX64RegisterXmmControlStatus:
            if (Fpu != NULL)
            {
                Value->Low = Fpu->DataPointer;
                Value->High = (ULONG64)Fpu->MxCsr |
                              ((ULONG64)Fpu->MxCsrMask << 32);
            }

            break;

        case HvRegisterVpIndex: Value->Low = Vp->Index; break;

        /* Root signals are not implemented */
        case HvRegisterVpRootSignalCount: Value->Low = 0; break;

        /* Last exit info */
        case ReactVRegisterInterceptReason:
            Value->Low = ReactVInterceptReason(Vmcb->Control.ExitCode);
            break;

        case ReactVRegisterInterceptInfo1:
            Value->Low = Vmcb->Control.ExitInfo1;
            break;

        case ReactVRegisterInterceptInfo2:
            Value->Low = Vmcb->Control.ExitInfo2;
            break;

        case ReactVRegisterInterceptLength:
            Value->Low = ReactVInterceptLength(Vmcb);
            break;

        case HvRegisterPendingInterruption:
            Value->Low = ReactVPendingInterruption(Vmcb);
            break;

        case HvRegisterInterruptState:
            if (Vmcb->Control.InterruptShadow & REACTV_SHADOW_INTERRUPT)
                Value->Low |= HV_INTERRUPT_STATE_SHADOW;

            if (Vp->NmiMasked)
                Value->Low |= HV_INTERRUPT_STATE_NMI_MASKED;

            break;

        case HvX64RegisterDeliverabilityNotifications:
            Value->Low = Vp->Deliverability;
            break;

        case HvRegisterExplicitSuspend: Value->Low = Vp->ExplicitSuspend; break;
        case HvRegisterInterceptSuspend: Value->Low = Vp->InterceptSuspend; break;
        case HvRegisterDispatchSuspend: Value->Low = Vp->DispatchSuspend; break;
        case HvRegisterEmulationHints: Value->Low = Vp->EmulationHints; break;

        case HvRegisterInternalActivityState:
            if (Vp->Apic.WaitingForStartup)
                Value->Low = HV_ACTIVITY_STARTUP_SUSPEND;

            break;

        default:
            /* Untracked registers read as zero */
            ReactVLogWarning("Untracked register %lx\n", Name);
            break;
    }

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * TRUE if the VP must not run.
 *
 * @remarks
 * Suspended by the root or waiting for SIPI. Either way the dispatch returns
 * blocked with nothing to report.
 */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVVpHeld(
    _In_ PREACTV_VP Vp)
{
    if (((Vp->DispatchSuspend | Vp->ExplicitSuspend) & HV_SUSPEND_SUSPENDED) != 0)
        return TRUE;

    return Vp->Apic.WaitingForStartup;
}

ULONG64
NTAPI
ReactVChildWriteRegister(
    _In_ PREACTV_VP Vp,
    _In_ ULONG Name,
    _In_ PHV_REGISTER_VALUE Value)
{
    PREACTV_VMCB Vmcb = ReactVVmcbOf(Vp);
    PREACTV_FPU_STATE Fpu = (PREACTV_FPU_STATE)(ULONG_PTR)Vp->FpuState;
    PVOID Vector;

    Vector = ReactVVectorRegister(Vp, Name);
    if (Vector != NULL)
    {
        RtlCopyMemory(Vector, Value, sizeof(*Value));
        return HV_STATUS_SUCCESS;
    }

    switch (Name)
    {
        case HvX64RegisterRax: Vmcb->Save.Rax = Value->Low; break;
        case HvX64RegisterRcx: Vp->Registers.Rcx = Value->Low; break;
        case HvX64RegisterRdx: Vp->Registers.Rdx = Value->Low; break;
        case HvX64RegisterRbx: Vp->Registers.Rbx = Value->Low; break;
        case HvX64RegisterRsp: Vmcb->Save.Rsp = Value->Low; break;
        case HvX64RegisterRbp: Vp->Registers.Rbp = Value->Low; break;
        case HvX64RegisterRsi: Vp->Registers.Rsi = Value->Low; break;
        case HvX64RegisterRdi: Vp->Registers.Rdi = Value->Low; break;
        case HvX64RegisterR8:  Vp->Registers.R8 = Value->Low; break;
        case HvX64RegisterR9:  Vp->Registers.R9 = Value->Low; break;
        case HvX64RegisterR10: Vp->Registers.R10 = Value->Low; break;
        case HvX64RegisterR11: Vp->Registers.R11 = Value->Low; break;
        case HvX64RegisterR12: Vp->Registers.R12 = Value->Low; break;
        case HvX64RegisterR13: Vp->Registers.R13 = Value->Low; break;
        case HvX64RegisterR14: Vp->Registers.R14 = Value->Low; break;
        case HvX64RegisterR15: Vp->Registers.R15 = Value->Low; break;
        case HvX64RegisterRip: Vmcb->Save.Rip = Value->Low; break;

        case HvX64RegisterRflags:
            /* RFLAGS bit 1 is reserved and always set */
            Vmcb->Save.Rflags = Value->Low | REACTV_RESET_RFLAGS;
            break;

        case HvX64RegisterCr0:
        case HvX64RegisterCr0Paged: Vmcb->Save.Cr0 = Value->Low; break;
        case HvX64RegisterCr2: Vmcb->Save.Cr2 = Value->Low; break;
        case HvX64RegisterCr3:
        case HvX64RegisterCr3Paged: Vmcb->Save.Cr3 = Value->Low; break;
        case HvX64RegisterCr4:
        case HvX64RegisterCr4Paged: Vmcb->Save.Cr4 = Value->Low; break;

        /* TPR is not used */
        case HvX64RegisterCr8Paged: break;
        case HvX64RegisterEfer:
            Vmcb->Save.Efer = Value->Low | AMD64_EFER_SVME;
            break;

        case HvX64RegisterPat: Vmcb->Save.GuestPat = Value->Low; break;

        case HvX64RegisterEs: ReactVRegisterToSegment(Value, &Vmcb->Save.Es); break;
        case HvX64RegisterCs: ReactVRegisterToSegment(Value, &Vmcb->Save.Cs); break;
        case HvX64RegisterSs: ReactVRegisterToSegment(Value, &Vmcb->Save.Ss); break;
        case HvX64RegisterDs: ReactVRegisterToSegment(Value, &Vmcb->Save.Ds); break;
        case HvX64RegisterFs: ReactVRegisterToSegment(Value, &Vmcb->Save.Fs); break;
        case HvX64RegisterGs: ReactVRegisterToSegment(Value, &Vmcb->Save.Gs); break;
        case HvX64RegisterLdtr: ReactVRegisterToSegment(Value, &Vmcb->Save.Ldtr); break;
        case HvX64RegisterTr: ReactVRegisterToSegment(Value, &Vmcb->Save.Tr); break;

        case HvX64RegisterGdtr: ReactVRegisterToTable(Value, &Vmcb->Save.Gdtr); break;
        case HvX64RegisterIdtr: ReactVRegisterToTable(Value, &Vmcb->Save.Idtr); break;

        case HvX64RegisterKernelGsBase: Vmcb->Save.KernelGsBase = Value->Low; break;
        case HvX64RegisterStar: Vmcb->Save.Star = Value->Low; break;
        case HvX64RegisterLstar: Vmcb->Save.LStar = Value->Low; break;
        case HvX64RegisterCstar: Vmcb->Save.CStar = Value->Low; break;
        case HvX64RegisterSfmask: Vmcb->Save.SfMask = Value->Low; break;
        case HvX64RegisterSysenterCs: Vmcb->Save.SysenterCs = Value->Low; break;
        case HvX64RegisterSysenterEsp: Vmcb->Save.SysenterEsp = Value->Low; break;
        case HvX64RegisterSysenterEip: Vmcb->Save.SysenterEip = Value->Low; break;

        case HvRegisterPendingInterruption:
            ReactVInject(Vmcb, Value->Low);
            break;

        case HvRegisterInterruptState:
            if (Value->Low & HV_INTERRUPT_STATE_SHADOW)
                Vmcb->Control.InterruptShadow |= REACTV_SHADOW_INTERRUPT;
            else
                Vmcb->Control.InterruptShadow &= ~REACTV_SHADOW_INTERRUPT;

            Vp->NmiMasked = (Value->Low & HV_INTERRUPT_STATE_NMI_MASKED) ? TRUE : FALSE;
            break;

        case HvX64RegisterDeliverabilityNotifications:
            Vp->Deliverability = Value->Low;
            ReactVWatchForDeliverability(Vp);
            break;

        case HvX64RegisterFpControlStatus:
            if (Fpu != NULL)
            {
                Fpu->ControlWord = (USHORT)Value->Low;
                Fpu->StatusWord = (USHORT)(Value->Low >> 16);
                Fpu->TagWord = (UCHAR)(Value->Low >> 32);
                Fpu->Opcode = (USHORT)(Value->Low >> 48);
                Fpu->InstructionPointer = Value->High;
            }

            break;

        case HvX64RegisterXmmControlStatus:
            if (Fpu != NULL)
            {
                Fpu->DataPointer = Value->Low;
                Fpu->MxCsr = (ULONG)Value->High;
            }

            break;

        case HvRegisterExplicitSuspend: Vp->ExplicitSuspend = Value->Low; break;
        case HvRegisterInterceptSuspend: Vp->InterceptSuspend = Value->Low; break;
        case HvRegisterDispatchSuspend: Vp->DispatchSuspend = Value->Low; break;
        case HvRegisterEmulationHints: Vp->EmulationHints = Value->Low; break;

        /* Clearing startup suspend means the root started the VP itself */
        case HvRegisterInternalActivityState:
            Vp->Apic.WaitingForStartup =
                (Value->Low & HV_ACTIVITY_STARTUP_SUSPEND) != 0;
            break;

        default:
            ReactVLogWarning("Unsupported register write %lx\n", Name);
            return HV_STATUS_INVALID_PARAMETER;
    }

    /* Force the CPU to reload all VMCB state */
    Vmcb->Control.VmcbClean = 0;

    return HV_STATUS_SUCCESS;
}
