/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SVM exit handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"

/*
 * Runs with GIF clear, on the core stack and page tables. The loader interface
 * is unusable here since the loader is now the guest.
 */

/* Instruction lengths, used without NRIP_SAVE */
#define REACTV_LENGTH_CPUID     2
#define REACTV_LENGTH_RDMSR     2
#define REACTV_LENGTH_WRMSR     2
#define REACTV_LENGTH_VMMCALL   3

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
ReactVAdvanceRip(
    _In_ PREACTV_VMCB Vmcb,
    _In_ ULONG Length)
{
    /* Emulated CPUs often lack NRIP_SAVE, so fall back to the fixed length */
    if ((ReactVCore.SvmFeatures & AMD64_SVM_FEATURE_NRIP_SAVE) &&
        (Vmcb->Control.NextRip != 0))
    {
        Vmcb->Save.Rip = Vmcb->Control.NextRip;
        return;
    }

    Vmcb->Save.Rip += Length;
}

static
VOID
NTAPI
ReactVInjectGeneralProtection(
    _In_ PREACTV_VMCB Vmcb)
{
    /* #GP is a fault, RIP is not advanced */
    Vmcb->Control.EventInject = REACTV_VECTOR_GP_FAULT |
                                (REACTV_EVENT_TYPE_EXCEPTION << REACTV_EVENT_TYPE_SHIFT) |
                                REACTV_EVENT_ERROR_VALID |
                                REACTV_EVENT_VALID;
}

static
VOID
NTAPI
ReactVHandleCpuid(
    _In_ PREACTV_VMCB Vmcb,
    _In_ PREACTV_GUEST_REGISTERS Registers)
{
    ULONG Values[4];
    int Result[4];
    ULONG Leaf, SubLeaf;

    Leaf = (ULONG)Vmcb->Save.Rax;
    SubLeaf = (ULONG)Registers->Rcx;

    __cpuidex(Result, (int)Leaf, (int)SubLeaf);
    Values[0] = (ULONG)Result[0];
    Values[1] = (ULONG)Result[1];
    Values[2] = (ULONG)Result[2];
    Values[3] = (ULONG)Result[3];

    ReactVHvCpuid(Leaf, SubLeaf, Values);

    Vmcb->Save.Rax = Values[0];
    Registers->Rbx = Values[1];
    Registers->Rcx = Values[2];
    Registers->Rdx = Values[3];

    ReactVAdvanceRip(Vmcb, REACTV_LENGTH_CPUID);
}

/**
 * @brief
 * Intercepts SIPI writes to the x2APIC ICR.
 *
 * @return
 * FALSE if the write is not a SIPI or the core could not start the AP. The
 * caller then writes the ICR itself.
 *
 * @remarks
 * An AP started by the root runs outside the core and faults on its first
 * hypercall, so the core starts it as a guest at the SIPI vector. INIT passes
 * through and leaves the AP in wait-for-SIPI.
 */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVHandleStartup(
    _In_ ULONG64 Value)
{
    ULONG Message = (ULONG)(Value >> REACTV_ICR_MESSAGE_SHIFT) & REACTV_ICR_MESSAGE_MASK;
    ULONG ApicId = (ULONG)(Value >> REACTV_ICR_DESTINATION_SHIFT);
    ULONG Vector = (ULONG)Value & REACTV_ICR_VECTOR_MASK;

    if (Message != REACTV_ICR_MESSAGE_STARTUP)
        return FALSE;

    /* The root sends two SIPIs, ignore the second */
    if (ReactVApIsLive(ApicId))
        return TRUE;

    if (!ReactVApStart(ApicId, Vector))
    {
        /* Pass the SIPI through; the AP runs outside the core */
        ReactVLogWarning("APIC %lu start failed, passing SIPI through\n",
                         ApicId);
        return FALSE;
    }

    return TRUE;
}

static
VOID
NTAPI
ReactVHandleMsr(
    _In_ PREACTV_VMCB Vmcb,
    _In_ PREACTV_GUEST_REGISTERS Registers)
{
    ULONG Index = (ULONG)Registers->Rcx;
    ULONG64 Value;

    /* EXITINFO1 is 1 for WRMSR, 0 for RDMSR */
    if (Vmcb->Control.ExitInfo1 != 0)
    {
        Value = ((ULONG64)(ULONG)Registers->Rdx << 32) | (ULONG)Vmcb->Save.Rax;

        /* ICR writes are checked for SIPI before reaching hardware */
        if (Index == REACTV_X2APIC_ICR)
        {
            if (!ReactVHandleStartup(Value))
                __writemsr(REACTV_X2APIC_ICR, Value);

            ReactVAdvanceRip(Vmcb, REACTV_LENGTH_WRMSR);
            return;
        }

        if (!ReactVHvWriteMsr(Index, Value))
        {
            /* Unknown MSR */
            ReactVInjectGeneralProtection(Vmcb);
            return;
        }

        ReactVAdvanceRip(Vmcb, REACTV_LENGTH_WRMSR);
        return;
    }

    if (!ReactVHvReadMsr(Index, &Value))
    {
        ReactVInjectGeneralProtection(Vmcb);
        return;
    }

    Vmcb->Save.Rax = (ULONG)Value;
    Registers->Rdx = (ULONG)(Value >> 32);

    ReactVAdvanceRip(Vmcb, REACTV_LENGTH_RDMSR);
}

static
VOID
NTAPI
ReactVHandleVmmcall(
    _In_ PREACTV_VMCB Vmcb,
    _In_ PREACTV_GUEST_STATE State)
{
    Vmcb->Save.Rax = ReactVHvHypercall(State->Registers.Rcx,
                                       State->Registers.Rdx,
                                       State->Registers.R8,
                                       Vmcb->Save.Cr3,
                                       State);

    ReactVAdvanceRip(Vmcb, REACTV_LENGTH_VMMCALL);
}

BOOLEAN
NTAPI
ReactVSvmHandleExit(
    _In_ ULONG64 VmcbPhysical,
    _In_ PREACTV_GUEST_STATE State)
{
    /* Identity mapped, VA equals PA */
    PREACTV_VMCB Vmcb = (PREACTV_VMCB)(ULONG_PTR)VmcbPhysical;

    switch (Vmcb->Control.ExitCode)
    {
        case REACTV_EXIT_CPUID:
            ReactVHandleCpuid(Vmcb, &State->Registers);
            break;

        case REACTV_EXIT_MSR:
            ReactVHandleMsr(Vmcb, &State->Registers);
            break;

        case REACTV_EXIT_VMMCALL:
            ReactVHandleVmmcall(Vmcb, State);
            break;

        case REACTV_EXIT_INVALID:
            /* VMRUN rejected the VMCB state */
            return FALSE;

        default:
            /* Unhandled exit, resume the root */
            break;
    }

    /* Deliver pending SynIC messages and events to the root before resuming */
    ReactVSynicDeliverToRoot(Vmcb);

    return TRUE;
}
