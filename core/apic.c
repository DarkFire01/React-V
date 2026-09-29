/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Per-VP virtual local APIC (x2APIC)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"

/*
 * x2APIC mode only: every register is an MSR, so rdmsr/wrmsr intercepts cover
 * it. xAPIC MMIO would need instruction decoding.
 */

/* FUNCTIONS ******************************************************************/

static
PREACTV_VMCB
NTAPI
ReactVApicVmcb(
    _In_ PREACTV_VP Vp)
{
    return (PREACTV_VMCB)(ULONG_PTR)Vp->Vmcb;
}

VOID
NTAPI
ReactVApicReset(
    _In_ PREACTV_VP Vp)
{
    PREACTV_VP_APIC Apic = &Vp->Apic;

    RtlZeroMemory(Apic, sizeof(*Apic));

    Apic->Id = Vp->Index;
    Apic->LogicalId = 1UL << (Vp->Index & 0x1F);
    Apic->Spurious = AMD64_APIC_SVR_RESET;
    Apic->Base = AMD64_APIC_DEFAULT_ADDRESS | AMD64_APIC_BASE_ENABLE;

    /* VP 0 is the BSP; the rest wait for INIT/SIPI */
    if (Vp->Index == 0)
        Apic->Base |= AMD64_APIC_BASE_BSP;
    else
        Apic->WaitingForStartup = TRUE;
}

/* Vector to bitmap word and bit */
static
VOID
NTAPI
ReactVApicWhere(
    _In_ ULONG Vector,
    _Out_ PULONG Word,
    _Out_ PULONG Bit)
{
    *Word = (Vector >> 5) & (REACTV_APIC_VECTOR_WORDS - 1);
    *Bit = 1UL << (Vector & 0x1F);
}

/* Highest set vector in a bitmap, or 0 */
static
ULONG
NTAPI
ReactVApicHighest(
    _In_reads_(REACTV_APIC_VECTOR_WORDS) const ULONG *Bits)
{
    ULONG Word = REACTV_APIC_VECTOR_WORDS;

    while (Word > 0)
    {
        ULONG Bit = 32;

        Word--;

        if (Bits[Word] == 0)
            continue;

        while (Bit > 0)
        {
            Bit--;

            if (Bits[Word] & (1UL << Bit))
                return (Word * 32) + Bit;
        }
    }

    return 0;
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVApicWaiting(
    _In_ PREACTV_VP Vp)
{
    ULONG Word;

    for (Word = 0; Word < REACTV_APIC_VECTOR_WORDS; Word++)
    {
        if (Vp->Apic.Requested[Word] != 0)
            return TRUE;
    }

    return FALSE;
}

/**
 * @brief
 * Moves the highest pending vector from IRR to ISR.
 *
 * @return
 * The vector, or zero if nothing is pending above the TPR.
 */
ULONG
NTAPI
ReactVApicTakePending(
    _In_ PREACTV_VP Vp)
{
    PREACTV_VP_APIC Apic = &Vp->Apic;
    ULONG Vector;
    ULONG Word;
    ULONG Bit;

    if (!ReactVApicWaiting(Vp))
        return 0;

    Vector = ReactVApicHighest(Apic->Requested);

    /* Priority class is vector bits 7:4, TPR bits 7:4 */
    if ((Vector >> 4) <= ((Apic->TaskPriority >> 4) & 0x0F))
        return 0;

    ReactVApicWhere(Vector, &Word, &Bit);

    Apic->Requested[Word] &= ~Bit;
    Apic->InService[Word] |= Bit;

    return Vector;
}

/* Sets a vector in the target VP's IRR */
static
VOID
NTAPI
ReactVApicRequest(
    _In_ PREACTV_VP Vp,
    _In_ ULONG Vector)
{
    ULONG Word;
    ULONG Bit;

    /* Vectors 0-15 are illegal for IPIs */
    if (Vector < 16)
        return;

    ReactVApicWhere(Vector, &Word, &Bit);

    Vp->Apic.Requested[Word] |= Bit;
}

/* EOI: clear the highest ISR bit */
static
VOID
NTAPI
ReactVApicEndOfInterrupt(
    _In_ PREACTV_VP Vp)
{
    PREACTV_VP_APIC Apic = &Vp->Apic;
    ULONG Vector = ReactVApicHighest(Apic->InService);
    ULONG Word;
    ULONG Bit;

    if (Vector == 0)
        return;

    ReactVApicWhere(Vector, &Word, &Bit);

    Apic->InService[Word] &= ~Bit;
}

/**
 * @brief
 * Handles a SIPI to a VP in wait-for-SIPI.
 *
 * @remarks
 * The VP starts in real mode at CS = vector << 8, IP = 0.
 */
static
VOID
NTAPI
ReactVApicStartup(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Target,
    _In_ ULONG Vector)
{
    PREACTV_VMCB Vmcb = ReactVApicVmcb(Target);

    if (!Target->Apic.WaitingForStartup)
        return;

    Target->Apic.WaitingForStartup = FALSE;

    Vmcb->Save.Cs.Selector = (USHORT)(Vector << 8);
    Vmcb->Save.Cs.Base = (ULONG64)Vector << 12;
    Vmcb->Save.Cs.Limit = 0xFFFF;
    Vmcb->Save.Rip = 0;
    Vmcb->Control.VmcbClean = 0;

    /* Signal last: the root thread may run the VP as soon as it is woken */
    ReactVSynicSignalRunnable(Partition->Id, Target->Index);
}

/* INIT: put the VP back in wait-for-SIPI */
static
VOID
NTAPI
ReactVApicInitialize(
    _In_ PREACTV_VP Target)
{
    /* INIT to the BSP is ignored */
    if (Target->Index == 0)
        return;

    Target->Apic.WaitingForStartup = TRUE;
}

/* Checks whether an ICR write targets this VP */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVApicAddressed(
    _In_ PREACTV_VP Sender,
    _In_ PREACTV_VP Target,
    _In_ ULONG Shorthand,
    _In_ ULONG Destination)
{
    switch (Shorthand)
    {
        case AMD64_APIC_SHORTHAND_SELF:
            return Target == Sender;

        case AMD64_APIC_SHORTHAND_ALL:
            return TRUE;

        case AMD64_APIC_SHORTHAND_OTHERS:
            return Target != Sender;

        default:
            break;
    }

    if (Destination == AMD64_APIC_DESTINATION_ALL)
        return TRUE;

    return Target->Apic.Id == Destination;
}

/**
 * @brief
 * Delivers an IPI from an ICR write.
 */
static
VOID
NTAPI
ReactVApicCommand(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Sender,
    _In_ ULONG64 Command)
{
    ULONG Vector = (ULONG)(Command & AMD64_APIC_ICR_VECTOR_MASK);
    ULONG Delivery = (ULONG)((Command >> AMD64_APIC_ICR_DELIVERY_SHIFT) &
                             AMD64_APIC_ICR_DELIVERY_MASK);
    ULONG Shorthand = (ULONG)((Command >> AMD64_APIC_ICR_SHORTHAND_SHIFT) &
                              AMD64_APIC_ICR_SHORTHAND_MASK);
    ULONG Destination = (ULONG)(Command >> AMD64_APIC_ICR_DESTINATION_SHIFT);
    ULONG Index;

    for (Index = 0; Index < REACTV_MAXIMUM_VPS; Index++)
    {
        PREACTV_VP Target = &Partition->Vp[Index];

        if (!Target->Created)
            continue;

        if (!ReactVApicAddressed(Sender, Target, Shorthand, Destination))
            continue;

        switch (Delivery)
        {
            case AMD64_APIC_DELIVERY_FIXED:
            case AMD64_APIC_DELIVERY_LOWEST:
                ReactVApicRequest(Target, Vector);
                break;

            case AMD64_APIC_DELIVERY_INIT:
                ReactVApicInitialize(Target);
                break;

            case AMD64_APIC_DELIVERY_STARTUP:
                ReactVApicStartup(Partition, Target, Vector);
                break;

            default:
                /* SMI, NMI and the rest are not supported */
                ReactVLogWarning("Partition %I64u VP %lu unsupported IPI delivery mode %lu\n",
                                 Partition->Id,
                                 Sender->Index,
                                 Delivery);
                break;
        }
    }
}

/**
 * @brief
 * Normalizes a synthetic ICR write to the x2APIC layout.
 *
 * @remarks
 * In x2APIC mode the destination is bits 63:32; in xAPIC mode it is bits 63:56,
 * matching the xAPIC ICR.
 */
static
ULONG64
NTAPI
ReactVApicSyntheticCommand(
    _In_ PREACTV_VP_APIC Apic,
    _In_ ULONG64 Value)
{
    if (Apic->Base & AMD64_APIC_BASE_EXTENDED)
        return Value;

    return (Value & 0xFFFFFFFF) |
           (((Value >> 56) & 0xFF) << AMD64_APIC_ICR_DESTINATION_SHIFT);
}

/**
 * @brief
 * Handles the synthetic EOI, TPR and ICR MSRs and the VP assist page MSR.
 *
 * @return
 * FALSE when the index is not one of them.
 */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVApicSyntheticMsr(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Index,
    _In_ BOOLEAN Write,
    _Inout_ PULONG64 Value)
{
    PREACTV_VP_APIC Apic = &Vp->Apic;

    switch (Index)
    {
        case HV_X64_MSR_EOI:
            /* Write only; reads return 0 */
            if (Write)
                ReactVApicEndOfInterrupt(Vp);
            else
                *Value = 0;

            return TRUE;

        case HV_X64_MSR_TPR:
            if (Write)
                Apic->TaskPriority = (ULONG)*Value & 0xFF;
            else
                *Value = Apic->TaskPriority;

            return TRUE;

        case HV_X64_MSR_ICR:
            if (!Write)
            {
                *Value = Apic->Command;
                return TRUE;
            }

            Apic->Command = *Value;
            ReactVApicCommand(Partition,
                              Vp,
                              ReactVApicSyntheticCommand(Apic, *Value));

            return TRUE;

        case HV_X64_MSR_VP_ASSIST_PAGE:
            /* Stored only; EOI avoidance is never offered, so the guest always EOIs */
            if (Write)
                Apic->AssistPage = *Value;
            else
                *Value = Apic->AssistPage;

            return TRUE;

        default:
            break;
    }

    return FALSE;
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVApicMsr(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Index,
    _In_ BOOLEAN Write,
    _Inout_ PULONG64 Value)
{
    PREACTV_VP_APIC Apic = &Vp->Apic;

    if ((Index >= HV_X64_MSR_EOI) && (Index <= HV_X64_MSR_VP_ASSIST_PAGE))
        return ReactVApicSyntheticMsr(Partition, Vp, Index, Write, Value);

    if (Index == AMD64_MSR_APIC_BASE)
    {
        if (Write)
            Apic->Base = *Value;
        else
            *Value = Apic->Base;

        return TRUE;
    }

    if ((Index < AMD64_X2APIC_FIRST) || (Index > AMD64_X2APIC_LAST))
        return FALSE;

    /* x2APIC MSRs do not exist until the guest enables x2APIC mode */
    if (!(Apic->Base & AMD64_APIC_BASE_EXTENDED))
        return FALSE;

    if ((Index >= AMD64_X2APIC_ISR_FIRST) && (Index < AMD64_X2APIC_ESR))
    {
        ULONG Word = (Index - AMD64_X2APIC_ISR_FIRST) & (REACTV_APIC_VECTOR_WORDS - 1);

        /* ISR, TMR and IRR are read only; writes are dropped */
        if (Write)
            return TRUE;

        if (Index < AMD64_X2APIC_TMR_FIRST)
            *Value = Apic->InService[Word];
        else if (Index < AMD64_X2APIC_IRR_FIRST)
            *Value = 0;
        else
            *Value = Apic->Requested[Word];

        return TRUE;
    }

    switch (Index)
    {
        case AMD64_X2APIC_ID:
            if (!Write)
                *Value = Apic->Id;

            return TRUE;

        case AMD64_X2APIC_VERSION:
            if (!Write)
                *Value = AMD64_APIC_VERSION_VALUE;

            return TRUE;

        case AMD64_X2APIC_TPR:
            if (Write)
                Apic->TaskPriority = (ULONG)*Value;
            else
                *Value = Apic->TaskPriority;

            return TRUE;

        case AMD64_X2APIC_PPR:
            if (!Write)
                *Value = ReactVApicHighest(Apic->InService) & 0xF0;

            return TRUE;

        case AMD64_X2APIC_EOI:
            if (Write)
                ReactVApicEndOfInterrupt(Vp);

            return TRUE;

        case AMD64_X2APIC_LDR:
            if (!Write)
                *Value = Apic->LogicalId;

            return TRUE;

        case AMD64_X2APIC_SVR:
            if (Write)
                Apic->Spurious = (ULONG)*Value;
            else
                *Value = Apic->Spurious;

            return TRUE;

        case AMD64_X2APIC_ESR:
            if (Write)
                Apic->ErrorStatus = 0;
            else
                *Value = Apic->ErrorStatus;

            return TRUE;

        case AMD64_X2APIC_ICR:
            if (!Write)
            {
                *Value = Apic->Command;
                return TRUE;
            }

            Apic->Command = *Value;
            ReactVApicCommand(Partition, Vp, *Value);

            return TRUE;

        case AMD64_X2APIC_SELF_IPI:
            if (Write)
                ReactVApicRequest(Vp, (ULONG)(*Value & AMD64_APIC_ICR_VECTOR_MASK));

            return TRUE;

        case AMD64_X2APIC_LVT_TIMER:
            if (Write)
                Apic->Timer = (ULONG)*Value;
            else
                *Value = Apic->Timer;

            return TRUE;

        case AMD64_X2APIC_LVT_ERROR:
            if (Write)
                Apic->Error = (ULONG)*Value;
            else
                *Value = Apic->Error;

            return TRUE;

        case AMD64_X2APIC_TIMER_INITIAL:
            if (Write)
                Apic->TimerInitial = (ULONG)*Value;
            else
                *Value = Apic->TimerInitial;

            return TRUE;

        case AMD64_X2APIC_TIMER_CURRENT:
            /* Timer is not emulated; current count reads as expired */
            if (!Write)
                *Value = 0;

            return TRUE;

        case AMD64_X2APIC_TIMER_DIVIDE:
            if (Write)
                Apic->TimerDivide = (ULONG)*Value;
            else
                *Value = Apic->TimerDivide;

            return TRUE;

        default:
            break;
    }

    /* Remaining registers (other LVT entries etc.) are not emulated: writes dropped, reads 0 */
    if (!Write)
        *Value = 0;

    return TRUE;
}

/* EOF */
