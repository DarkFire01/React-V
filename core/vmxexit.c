/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VMX exit handling for the root
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcs.h"
#include "arch/amd64/vmxasm.h"

/*
 * VMX counterpart of exit.c. Unlike SVM, the exit reports the instruction
 * length, and guest RAX is saved on the stack rather than in the VMCB.
 */

/* Assembly stub encodings must match the VMCS field definitions */
C_ASSERT(REACTV_VMCS_GUEST_RSP_ENC == REACTV_VMCS_GUEST_RSP);
C_ASSERT(REACTV_VMCS_HOST_RSP_ENC == REACTV_VMCS_HOST_RSP);

/* VM-entry interruption information, Intel SDM vol. 3 table 24-15 */
#define REACTV_VMX_EVENT_VALID          0x80000000
#define REACTV_VMX_EVENT_ERROR_VALID    0x00000800
#define REACTV_VMX_EVENT_HARDWARE       (3 << 8)

#define REACTV_VECTOR_GP_FAULT          13

/* FUNCTIONS ******************************************************************/

/* Skip the exiting instruction using the VMCS exit instruction length */
static
VOID
NTAPI
ReactVVmxAdvanceRip(VOID)
{
    ULONG64 Rip = ReactVVmxRead(REACTV_VMCS_GUEST_RIP);
    ULONG64 Length = ReactVVmxRead(REACTV_VMCS_EXIT_INSTRUCTION_LEN);

    ReactVVmxWrite(REACTV_VMCS_GUEST_RIP, Rip + Length);
}

static
VOID
NTAPI
ReactVVmxInjectGeneralProtection(VOID)
{
    /* Faulting injection, RIP is not advanced */
    ReactVVmxWrite(REACTV_VMCS_ENTRY_EXCEPTION_CODE, 0);
    ReactVVmxWrite(REACTV_VMCS_ENTRY_INTERRUPT_INFO,
                   REACTV_VECTOR_GP_FAULT |
                   REACTV_VMX_EVENT_HARDWARE |
                   REACTV_VMX_EVENT_ERROR_VALID |
                   REACTV_VMX_EVENT_VALID);
}

static
VOID
NTAPI
ReactVVmxHandleCpuid(
    _In_ PREACTV_VMX_GUEST_STATE State)
{
    ULONG Values[4];
    int Result[4];
    ULONG Leaf, SubLeaf;

    Leaf = (ULONG)State->Rax;
    SubLeaf = (ULONG)State->Registers.Rcx;

    __cpuidex(Result, (int)Leaf, (int)SubLeaf);
    Values[0] = (ULONG)Result[0];
    Values[1] = (ULONG)Result[1];
    Values[2] = (ULONG)Result[2];
    Values[3] = (ULONG)Result[3];

    ReactVHvCpuid(Leaf, SubLeaf, Values);

    State->Rax = Values[0];
    State->Registers.Rbx = Values[1];
    State->Registers.Rcx = Values[2];
    State->Registers.Rdx = Values[3];

    ReactVVmxAdvanceRip();
}

static
VOID
NTAPI
ReactVVmxHandleReadMsr(
    _In_ PREACTV_VMX_GUEST_STATE State)
{
    ULONG Index = (ULONG)State->Registers.Rcx;
    ULONG64 Value;

    if (!ReactVHvReadMsr(Index, &Value))
    {
        ReactVVmxInjectGeneralProtection();
        return;
    }

    State->Rax = (ULONG)Value;
    State->Registers.Rdx = (ULONG)(Value >> 32);

    ReactVVmxAdvanceRip();
}

static
VOID
NTAPI
ReactVVmxHandleWriteMsr(
    _In_ PREACTV_VMX_GUEST_STATE State)
{
    ULONG Index = (ULONG)State->Registers.Rcx;
    ULONG64 Value;

    Value = ((ULONG64)(ULONG)State->Registers.Rdx << 32) | (ULONG)State->Rax;

    if (!ReactVHvWriteMsr(Index, Value))
    {
        /* Unhandled MSR */
        ReactVVmxInjectGeneralProtection();
        return;
    }

    ReactVVmxAdvanceRip();
}

static
VOID
NTAPI
ReactVVmxHandleVmcall(
    _In_ PREACTV_VMX_GUEST_STATE State)
{
    /* REACTV_VMX_GUEST_STATE matches the REACTV_GUEST_STATE layout exit.c passes */
    State->Rax = ReactVHvHypercall(State->Registers.Rcx,
                                   State->Registers.Rdx,
                                   State->Registers.R8,
                                   ReactVVmxRead(REACTV_VMCS_GUEST_CR3),
                                   (PREACTV_GUEST_STATE)State);

    ReactVVmxAdvanceRip();
}

BOOLEAN
NTAPI
ReactVVmxHandleExit(
    _In_ PREACTV_VMX_GUEST_STATE State)
{
    ULONG64 Reason = ReactVVmxRead(REACTV_VMCS_EXIT_REASON);

    /* VM-entry failure: guest state is unchanged, re-entering would fail again */
    if (Reason & REACTV_VMX_EXIT_ENTRY_FAILURE)
    {
        ReactVLogError("VM entry failed, reason %I64x, error %I64x\n",
                       Reason & 0xFFFF,
                       ReactVVmxRead(REACTV_VMCS_INSTRUCTION_ERROR));
        return FALSE;
    }

    switch (Reason & 0xFFFF)
    {
        case REACTV_VMX_EXIT_CPUID:
            ReactVVmxHandleCpuid(State);
            break;

        case REACTV_VMX_EXIT_RDMSR:
            ReactVVmxHandleReadMsr(State);
            break;

        case REACTV_VMX_EXIT_WRMSR:
            ReactVVmxHandleWriteMsr(State);
            break;

        case REACTV_VMX_EXIT_VMCALL:
            ReactVVmxHandleVmcall(State);
            break;

        default:
            /* Unhandled exit, resume the root */
            break;
    }

    return TRUE;
}
