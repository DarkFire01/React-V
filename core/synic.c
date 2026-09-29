/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SynIC, synthetic timers, APIC and reference time MSRs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The root stack refuses a hypervisor that lacks the reference time counter,
 * SynIC, synthetic timers and the synthetic APIC MSRs.
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"

/* PIT, used to calibrate the TSC */
#define REACTV_PIT_CHANNEL2     0x42
#define REACTV_PIT_COMMAND      0x43
#define REACTV_PIT_GATE         0x61

/* Channel 2, low byte then high, interrupt on terminal count, binary */
#define REACTV_PIT_SETUP        0xB0

/* Gate 2 on, speaker off */
#define REACTV_PIT_GATE_ON      0x01
#define REACTV_PIT_GATE_SPEAKER 0x02

/* Channel 2 output, set on terminal count */
#define REACTV_PIT_GATE_OUTPUT  0x20

#define REACTV_PIT_FREQUENCY    1193182

/* PIT ticks to calibrate over */
#define REACTV_PIT_TICKS        0x2000

/* Reference time counter units */
#define REACTV_100NS_PER_SECOND 10000000

/* Local APIC, backing the synthetic APIC MSRs */
#define REACTV_APIC_BASE_MSR    0x0000001B
#define REACTV_APIC_BASE_MASK   0x0000000FFFFFF000ULL
#define REACTV_APIC_EXTENDED    0x0000000000000400ULL
#define REACTV_APIC_ENABLE      0x0000000000000800ULL
#define REACTV_CPUID_X2APIC     (1 << 21)
#define REACTV_APIC_TPR         0x0080
#define REACTV_APIC_EOI         0x00B0
#define REACTV_APIC_ICR_LOW     0x0300
#define REACTV_APIC_ICR_HIGH    0x0310

/* x2APIC MSR equivalents */
#define REACTV_X2APIC_TPR       0x00000808
#define REACTV_X2APIC_EOI       0x0000080B
#define REACTV_X2APIC_ICR       0x00000830

/* TYPES **********************************************************************/

typedef struct _REACTV_SYNIC
{
    ULONG64 Control;
    ULONG64 EventFlagsPage;
    ULONG64 MessagePage;
    ULONG64 RetargetPage;
    ULONG64 Sint[HV_SYNIC_SINT_COUNT];
} REACTV_SYNIC, *PREACTV_SYNIC;

typedef struct _REACTV_STIMER
{
    ULONG64 Config;
    ULONG64 Count;
} REACTV_STIMER, *PREACTV_STIMER;

/* GLOBALS ********************************************************************/

static REACTV_SYNIC ReactVSynic;
static REACTV_STIMER ReactVStimer[HV_SYNIC_STIMER_COUNT];

/* Reference time base TSC and TSC ticks per 100ns */
static ULONG64 ReactVTimeBase;
static ULONG64 ReactVCyclesPer100ns;

/* Measured TSC frequency, reported through HV_X64_MSR_TSC_FREQUENCY */
static ULONG64 ReactVCycleFrequency;

static ULONG64 ReactVApicBase;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Measures the TSC frequency against PIT channel 2.
 *
 * @return
 * TSC ticks per second, or zero if the PIT never counted down.
 *
 * @remarks
 * Channel 2 gate and output are both on port 0x61, so no interrupt is needed.
 */
static
ULONG64
NTAPI
ReactVMeasureCycles(VOID)
{
    ULONG64 Start;
    ULONG64 Elapsed;
    ULONG Spins;
    UCHAR Gate;

    /* Speaker off, gate 2 on */
    Gate = __inbyte(REACTV_PIT_GATE);
    Gate = (Gate & ~REACTV_PIT_GATE_SPEAKER) | REACTV_PIT_GATE_ON;
    __outbyte(REACTV_PIT_GATE, Gate);

    __outbyte(REACTV_PIT_COMMAND, REACTV_PIT_SETUP);
    __outbyte(REACTV_PIT_CHANNEL2, REACTV_PIT_TICKS & 0xFF);
    __outbyte(REACTV_PIT_CHANNEL2, REACTV_PIT_TICKS >> 8);

    Start = __rdtsc();

    /* Bounded so a machine without a PIT does not hang here */
    for (Spins = 0; Spins < 0x10000000; Spins++)
    {
        if (__inbyte(REACTV_PIT_GATE) & REACTV_PIT_GATE_OUTPUT)
            break;
    }

    Elapsed = __rdtsc() - Start;

    if (Spins == 0x10000000)
        return 0;

    return (Elapsed * REACTV_PIT_FREQUENCY) / REACTV_PIT_TICKS;
}

/**
 * @brief
 * Switches the root's local APIC to x2APIC mode.
 *
 * @return
 * TRUE if the APIC is in x2APIC mode on return.
 *
 * @remarks
 * The core must intercept INIT/SIPI to own the APs. In x2APIC mode the ICR is
 * MSR 0x830 and can be intercepted, while the xAPIC ICR is an MMIO store that
 * cannot. The root keeps the mode it finds, and x2APIC cannot be left without
 * a reset, so this runs before any xAPIC page access.
 */
static
BOOLEAN
NTAPI
ReactVRootApicToExtended(VOID)
{
    ULONG64 Base;
    int Registers[4];

    __cpuid(Registers, 1);
    if (!(Registers[2] & REACTV_CPUID_X2APIC))
    {
        ReactVLogWarning("No x2APIC support, "
                         "APs stay with the root\n");
        return FALSE;
    }

    Base = __readmsr(REACTV_APIC_BASE_MSR);
    if (Base & REACTV_APIC_EXTENDED)
        return TRUE;

    /* x2APIC requires the global enable bit, so set both */
    __writemsr(REACTV_APIC_BASE_MSR, Base | REACTV_APIC_ENABLE | REACTV_APIC_EXTENDED);

    Base = __readmsr(REACTV_APIC_BASE_MSR);
    if (!(Base & REACTV_APIC_EXTENDED))
    {
        ReactVLogWarning("x2APIC enable failed\n");
        return FALSE;
    }

    ReactVLogTrace("Root APIC in x2APIC mode, base %I64x\n", Base);
    return TRUE;
}

/**
 * @brief
 * Initializes the SynIC state and calibrates reference time.
 *
 * @return
 * FALSE if reference time is unavailable; the matching privilege must then not
 * be advertised.
 */
BOOLEAN
NTAPI
ReactVSynicInitialize(VOID)
{
    ULONG64 Cycles;
    ULONG Index;

    /* Must precede the APIC base read below */
    ReactVRootApicToExtended();

    RtlZeroMemory(&ReactVSynic, sizeof(ReactVSynic));
    RtlZeroMemory(&ReactVStimer, sizeof(ReactVStimer));

    for (Index = 0; Index < HV_SYNIC_SINT_COUNT; Index++)
        ReactVSynic.Sint[Index] = HV_SYNIC_SINT_RESET;

    ReactVApicBase = ReactVCore.Loader->System->ReadMsr(REACTV_APIC_BASE_MSR);

    Cycles = ReactVMeasureCycles();
    if (Cycles == 0)
    {
        ReactVLogWarning("PIT not responding, no reference time\n");
        return FALSE;
    }

    ReactVCyclesPer100ns = Cycles / REACTV_100NS_PER_SECOND;
    if (ReactVCyclesPer100ns == 0)
    {
        ReactVLogWarning("TSC at %I64u Hz, too slow for reference time\n",
                         Cycles);
        return FALSE;
    }

    ReactVCycleFrequency = Cycles;
    ReactVTimeBase = __rdtsc();

    ReactVLogTrace("TSC %I64u Hz, %I64u ticks per 100ns\n",
                   Cycles,
                   ReactVCyclesPer100ns);

    /* xAPIC mode: map the APIC MMIO page */
    if (!(ReactVApicBase & REACTV_APIC_EXTENDED))
    {
        ULONG64 Page = ReactVApicBase & REACTV_APIC_BASE_MASK;

        if (!NT_SUCCESS(ReactVMapCoreDevice(Page, Page, AMD64_PAGE_SIZE)))
        {
            ReactVLogWarning("Failed to map local APIC page\n");
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * @brief
 * Reads the partition reference time counter.
 *
 * @return
 * 100ns units since the core was initialized.
 */
ULONG64
NTAPI
ReactVReferenceTime(VOID)
{
    if (ReactVCyclesPer100ns == 0)
        return 0;

    return (__rdtsc() - ReactVTimeBase) / ReactVCyclesPer100ns;
}

/**
 * @brief
 * Returns the measured TSC frequency.
 *
 * @return
 * TSC ticks per second, or zero if calibration failed.
 */
ULONG64
NTAPI
ReactVCyclesPerSecond(VOID)
{
    return ReactVCycleFrequency;
}

/* Local APIC register access in either xAPIC or x2APIC mode */
static
ULONG
NTAPI
ReactVApicRead(
    _In_ ULONG Offset,
    _In_ ULONG ExtendedIndex)
{
    if (ReactVApicBase & REACTV_APIC_EXTENDED)
        return (ULONG)__readmsr(ExtendedIndex);

    return *(volatile ULONG *)(ULONG_PTR)((ReactVApicBase & REACTV_APIC_BASE_MASK) + Offset);
}

static
VOID
NTAPI
ReactVApicWrite(
    _In_ ULONG Offset,
    _In_ ULONG ExtendedIndex,
    _In_ ULONG Value)
{
    if (ReactVApicBase & REACTV_APIC_EXTENDED)
    {
        __writemsr(ExtendedIndex, Value);
        return;
    }

    *(volatile ULONG *)(ULONG_PTR)((ReactVApicBase & REACTV_APIC_BASE_MASK) + Offset) = Value;
}

/* Root SynIC helpers, defined below */
static
VOID
NTAPI
ReactVSynicRootFinished(VOID);

static
ULONG64
NTAPI
ReactVSynicRootWanted(
    _In_ ULONG Sint);

static
ULONG64
NTAPI
ReactVSynicTellRoot(
    _In_ ULONG Sint);

static
ULONG64
NTAPI
ReactVRootMessageSlot(
    _In_ ULONG Sint);

/* Maps and zeroes a SynIC page when its enable bit is set */
static
BOOLEAN
NTAPI
ReactVSynicMapPage(
    _In_ ULONG64 Value,
    _Out_ ULONG64 *Stored)
{
    ULONG64 Page = Value & HV_SYNIC_PAGE_MASK;
    PUCHAR Mapped;
    ULONG Index;

    if (!(Value & HV_SYNIC_PAGE_ENABLE))
    {
        *Stored = Value;
        return TRUE;
    }

    if (!NT_SUCCESS(ReactVMapCoreRange(Page, Page, AMD64_PAGE_SIZE)))
    {
        ReactVLogWarning("Failed to map SynIC page %I64x\n", Page);
        return FALSE;
    }

    /* A newly enabled SynIC page starts zeroed */
    Mapped = (PUCHAR)(ULONG_PTR)Page;
    for (Index = 0; Index < AMD64_PAGE_SIZE; Index++)
        Mapped[Index] = 0;

    *Stored = Value;

    return TRUE;
}

/**
 * @brief
 * Allocates a root SynIC page on first read of its MSR.
 *
 * @remarks
 * The root reads SIEFP/SIMP/SIRBP and maps what it finds instead of supplying
 * its own pages, so they come from the core page pool.
 *
 * @return
 * The MSR value, or zero if no page is available.
 */
static
ULONG64
NTAPI
ReactVRootControllerPage(
    _Inout_ ULONG64 *Stored)
{
    ULONG64 Page;

    if (*Stored != 0)
        return *Stored;

    if (!NT_SUCCESS(ReactVTakeCorePage(&Page)))
    {
        ReactVLogWarning("No core page for root SynIC\n");
        return 0;
    }

    if (!ReactVSynicMapPage(Page | HV_SYNIC_PAGE_ENABLE, Stored))
        return 0;

    ReactVLogTrace("Root SynIC page allocated at %I64x\n", Page);

    return *Stored;
}

/**
 * @brief
 * Reads a root synthetic MSR.
 *
 * @return
 * FALSE if the MSR is not handled here; the caller injects #GP.
 */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicReadMsr(
    _In_ ULONG Index,
    _Out_ ULONG64 *Value)
{
    if ((Index >= HV_X64_MSR_SINT0) && (Index <= HV_X64_MSR_SINT15))
    {
        *Value = ReactVSynic.Sint[Index - HV_X64_MSR_SINT0];
        return TRUE;
    }

    if ((Index >= HV_X64_MSR_STIMER0_CONFIG) && (Index <= HV_X64_MSR_STIMER3_COUNT))
    {
        ULONG Timer = (Index - HV_X64_MSR_STIMER0_CONFIG) / 2;

        if ((Index - HV_X64_MSR_STIMER0_CONFIG) & 1)
            *Value = ReactVStimer[Timer].Count;
        else
            *Value = ReactVStimer[Timer].Config;

        return TRUE;
    }

    switch (Index)
    {
        case HV_X64_MSR_TIME_REF_COUNT:
            *Value = ReactVReferenceTime();
            return TRUE;

        case HV_X64_MSR_TSC_FREQUENCY:
            /* PIT calibrated, not the CPUID reported value */
            *Value = ReactVCycleFrequency;
            return TRUE;

        case HV_X64_MSR_SCONTROL:
            *Value = ReactVSynic.Control;
            return TRUE;

        case HV_X64_MSR_SVERSION:
            *Value = HV_SYNIC_VERSION;
            return TRUE;

        case HV_X64_MSR_SIEFP:
            *Value = ReactVRootControllerPage(&ReactVSynic.EventFlagsPage);
            return TRUE;

        case HV_X64_MSR_SIMP:
            *Value = ReactVRootControllerPage(&ReactVSynic.MessagePage);
            return TRUE;

        case HV_X64_MSR_SIRBP:
            *Value = ReactVRootControllerPage(&ReactVSynic.RetargetPage);
            return TRUE;

        case HV_X64_MSR_TPR:
            *Value = ReactVApicRead(REACTV_APIC_TPR, REACTV_X2APIC_TPR);
            return TRUE;

        case HV_X64_MSR_ICR:
            *Value = ReactVApicRead(REACTV_APIC_ICR_LOW, REACTV_X2APIC_ICR);

            if (!(ReactVApicBase & REACTV_APIC_EXTENDED))
            {
                *Value |= (ULONG64)ReactVApicRead(REACTV_APIC_ICR_HIGH,
                                                  REACTV_X2APIC_ICR) << 32;
            }

            return TRUE;

        case HV_X64_MSR_EOM:
        case HV_X64_MSR_EOI:
            /* Write only, reads return zero */
            *Value = 0;
            return TRUE;

        default:
            break;
    }

    return FALSE;
}

/**
 * @brief
 * Writes a root synthetic MSR.
 *
 * @return
 * FALSE if the MSR is not handled here or is read only.
 */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicWriteMsr(
    _In_ ULONG Index,
    _In_ ULONG64 Value)
{
    if ((Index >= HV_X64_MSR_SINT0) && (Index <= HV_X64_MSR_SINT15))
    {
        ReactVSynic.Sint[Index - HV_X64_MSR_SINT0] = Value;

        ReactVLogTrace("Root SINT %lu vector %lu%s%s\n",
                       Index - HV_X64_MSR_SINT0,
                       (ULONG)(Value & HV_SYNIC_SINT_VECTOR_MASK),
                       (Value & HV_SYNIC_SINT_MASKED) ? ", masked" : "",
                       (Value & HV_SYNIC_SINT_PROXY) ? ", proxied" : "");

        return TRUE;
    }

    if ((Index >= HV_X64_MSR_STIMER0_CONFIG) && (Index <= HV_X64_MSR_STIMER3_COUNT))
    {
        ULONG Timer = (Index - HV_X64_MSR_STIMER0_CONFIG) / 2;

        if ((Index - HV_X64_MSR_STIMER0_CONFIG) & 1)
            ReactVStimer[Timer].Count = Value;
        else
            ReactVStimer[Timer].Config = Value;

        return TRUE;
    }

    switch (Index)
    {
        case HV_X64_MSR_SCONTROL:
            ReactVSynic.Control = Value;
            ReactVLogTrace("Root SCONTROL %I64x\n", Value);
            return TRUE;

        case HV_X64_MSR_SIEFP:
            ReactVLogTrace("Root SIEFP %I64x\n", Value);
            return ReactVSynicMapPage(Value, &ReactVSynic.EventFlagsPage);

        case HV_X64_MSR_SIMP:
            ReactVLogTrace("Root SIMP %I64x\n", Value);
            return ReactVSynicMapPage(Value, &ReactVSynic.MessagePage);

        case HV_X64_MSR_SIRBP:
            return ReactVSynicMapPage(Value, &ReactVSynic.RetargetPage);

        case HV_X64_MSR_EOM:
            /* Deliver the next queued message, if any */
            ReactVSynicRootFinished();
            return TRUE;

        case HV_X64_MSR_EOI:
            ReactVApicWrite(REACTV_APIC_EOI, REACTV_X2APIC_EOI, 0);
            return TRUE;

        case HV_X64_MSR_TPR:
            ReactVApicWrite(REACTV_APIC_TPR, REACTV_X2APIC_TPR, (ULONG)Value);
            return TRUE;

        case HV_X64_MSR_ICR:
            if (ReactVApicBase & REACTV_APIC_EXTENDED)
            {
                __writemsr(REACTV_X2APIC_ICR, Value);
                return TRUE;
            }

            /* High half first, the low write sends the IPI */
            ReactVApicWrite(REACTV_APIC_ICR_HIGH, REACTV_X2APIC_ICR, (ULONG)(Value >> 32));
            ReactVApicWrite(REACTV_APIC_ICR_LOW, REACTV_X2APIC_ICR, (ULONG)Value);
            return TRUE;

        case HV_X64_MSR_SVERSION:
        case HV_X64_MSR_TIME_REF_COUNT:
            /* Read only */
            return FALSE;

        default:
            break;
    }

    return FALSE;
}

/* CHILD SYNIC ****************************************************************/

/* Per VP SynIC of a child partition, emulated in the core */

/* Per SINT slot size in the message and event flags pages */
#define REACTV_SYNIC_MESSAGE_SIZE   256

/* MessageFlags bit: another message is pending behind this slot */
#define REACTV_MESSAGE_PENDING      0x01

static
PREACTV_VP_SYNIC
NTAPI
ReactVSynicOf(
    _In_ PREACTV_VP Vp)
{
    return &Vp->Synic;
}

/* Defined with the timers below */
static
VOID
NTAPI
ReactVTimerArm(
    _In_ PREACTV_VP_SYNIC Synic,
    _In_ ULONG Timer);

/* GPA of a SINT's message slot, or zero if SIMP is disabled */
static
ULONG64
NTAPI
ReactVSynicMessageSlot(
    _In_ PREACTV_VP_SYNIC Synic,
    _In_ ULONG Sint)
{
    if (!(Synic->MessagePage & HV_SYNIC_PAGE_ENABLE))
        return 0;

    return (Synic->MessagePage & HV_SYNIC_PAGE_MASK) +
           ((ULONG64)Sint * REACTV_SYNIC_MESSAGE_SIZE);
}

/**
 * @brief
 * Writes the VMMCALL stub into the guest hypercall page.
 *
 * @remarks
 * The GPA may not be backed yet when the MSR is written, so a failed write is
 * retried before the next VP run.
 */
VOID
NTAPI
ReactVSynicWriteCallPage(
    _In_ PREACTV_PARTITION Partition)
{
    static const UCHAR Stub[] = { 0x0F, 0x01, 0xD9, 0xC3 };

    Partition->HypercallStubWanted = FALSE;

    if (!(Partition->HypercallMsr & HV_X64_MSR_HYPERCALL_ENABLE))
        return;

    if (ReactVGuestWrite(Partition,
                         Partition->HypercallMsr & HV_SYNIC_PAGE_MASK,
                         Stub,
                         sizeof(Stub)))
    {
        return;
    }

    /* Not backed yet, retry before the next run */
    Partition->HypercallStubWanted = TRUE;
}

/**
 * @brief
 * Handles a child partition synthetic MSR access from the VP's own SynIC.
 *
 * @return
 * FALSE if the MSR is not handled here and should be forwarded to the root.
 */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicChildMsr(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Index,
    _In_ BOOLEAN Write,
    _Inout_ PULONG64 Value)
{
    PREACTV_VP_SYNIC Synic = ReactVSynicOf(Vp);

    if ((Index >= HV_X64_MSR_SINT0) && (Index <= HV_X64_MSR_SINT15))
    {
        ULONG Which = Index - HV_X64_MSR_SINT0;

        if (Write)
            Synic->Sint[Which] = *Value;
        else
            *Value = Synic->Sint[Which];

        return TRUE;
    }

    /* Each timer is a CONFIG/COUNT MSR pair */
    if ((Index >= HV_X64_MSR_STIMER0_CONFIG) && (Index <= HV_X64_MSR_STIMER3_COUNT))
    {
        ULONG Timer = (Index - HV_X64_MSR_STIMER0_CONFIG) / 2;
        BOOLEAN IsCount = ((Index - HV_X64_MSR_STIMER0_CONFIG) & 1) != 0;

        if (!Write)
        {
            *Value = IsCount ? Synic->TimerCount[Timer]
                             : Synic->TimerConfig[Timer];

            return TRUE;
        }

        if (IsCount)
        {
            Synic->TimerCount[Timer] = *Value;

            /* AutoEnable: writing COUNT sets Enable */
            if (Synic->TimerConfig[Timer] & HV_STIMER_AUTO_ENABLE)
                Synic->TimerConfig[Timer] |= HV_STIMER_ENABLE;
        }
        else
        {
            Synic->TimerConfig[Timer] = *Value;
        }

        ReactVTimerArm(Synic, Timer);

        return TRUE;
    }

    switch (Index)
    {
        case HV_X64_MSR_GUEST_OS_ID:
            if (Write)
                Partition->GuestOsId = *Value;
            else
                *Value = Partition->GuestOsId;

            return TRUE;

        case HV_X64_MSR_VP_INDEX:
            if (!Write)
                *Value = Vp->Index;

            return TRUE;

        case HV_X64_MSR_HYPERCALL:
            /* Stub page in guest memory hides the VMCALL/VMMCALL choice from the guest */
            if (!Write)
            {
                *Value = Partition->HypercallMsr;
                return TRUE;
            }

            Partition->HypercallMsr = *Value;

            if (*Value & HV_X64_MSR_HYPERCALL_ENABLE)
                ReactVSynicWriteCallPage(Partition);

            return TRUE;

        case HV_X64_MSR_TIME_REF_COUNT:
            if (!Write)
                *Value = ReactVReferenceTime();

            return TRUE;

        case HV_X64_MSR_SVERSION:
            if (!Write)
                *Value = HV_SYNIC_VERSION;

            return TRUE;

        case HV_X64_MSR_SCONTROL:
            if (Write)
                Synic->Control = *Value;
            else
                *Value = Synic->Control;

            return TRUE;

        case HV_X64_MSR_SIMP:
            if (Write)
                Synic->MessagePage = *Value;
            else
                *Value = Synic->MessagePage;

            return TRUE;

        case HV_X64_MSR_SIEFP:
            if (Write)
                Synic->EventPage = *Value;
            else
                *Value = Synic->EventPage;

            return TRUE;

        case HV_X64_MSR_EOM:
            /* Clear the serviced slot's message type so the next one can be posted */
            if (Write)
            {
                ULONG64 Slot = ReactVSynicMessageSlot(Synic, Synic->Serviced);
                ULONG Empty = 0;

                if (Slot != 0)
                    (VOID)ReactVGuestWrite(Partition, Slot, &Empty, sizeof(Empty));
            }

            return TRUE;

        default:
            break;
    }

    return FALSE;
}

/**
 * @brief
 * Dequeues the lowest pending SINT of a VP.
 *
 * @return
 * The SINT vector, or zero if nothing unmasked is pending.
 */
ULONG
NTAPI
ReactVSynicTakePending(
    _In_ PREACTV_VP Vp)
{
    PREACTV_VP_SYNIC Synic = ReactVSynicOf(Vp);
    ULONG Sint;

    for (Sint = 0; Sint < HV_SYNIC_SINT_COUNT; Sint++)
    {
        ULONG64 Setting = Synic->Sint[Sint];

        if (!(Synic->Pending & (1UL << Sint)))
            continue;

        Synic->Pending &= ~(1UL << Sint);

        if (Setting & HV_SYNIC_SINT_MASKED)
            continue;

        /* EOM clears this SINT's slot */
        Synic->Serviced = Sint;

        return (ULONG)(Setting & HV_SYNIC_SINT_VECTOR_MASK);
    }

    return 0;
}

/**
 * @brief
 * Posts a message to a VP's SIMP slot and marks the SINT pending.
 *
 * @return
 * HV_STATUS_INSUFFICIENT_BUFFERS if the slot is still occupied; the caller retries.
 */
ULONG64
NTAPI
ReactVSynicPostMessage(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Sint,
    _In_ PHV_MESSAGE Message)
{
    PREACTV_VP_SYNIC Synic = ReactVSynicOf(Vp);
    ULONG64 Slot = ReactVSynicMessageSlot(Synic, Sint);
    ULONG Type = 0;

    if (!(Synic->Control & HV_SYNIC_CONTROL_ENABLE) || (Slot == 0))
        return HV_STATUS_INVALID_SYNIC_STATE;

    if (Synic->Sint[Sint] & HV_SYNIC_SINT_MASKED)
        return HV_STATUS_INVALID_SYNIC_STATE;

    if (!ReactVGuestRead(Partition, Slot, &Type, sizeof(Type)))
        return HV_STATUS_INVALID_SYNIC_STATE;

    /* Slot busy: set MessagePending so the guest writes EOM */
    if (Type != 0)
    {
        UCHAR Flags = REACTV_MESSAGE_PENDING;

        (VOID)ReactVGuestWrite(Partition,
                               Slot + FIELD_OFFSET(HV_MESSAGE, MessageFlags),
                               &Flags,
                               sizeof(Flags));

        return HV_STATUS_INSUFFICIENT_BUFFERS;
    }

    if (!ReactVGuestWrite(Partition, Slot, Message, REACTV_SYNIC_MESSAGE_SIZE))
        return HV_STATUS_INVALID_SYNIC_STATE;

    Synic->Pending |= 1UL << Sint;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Sets an event flag in a VP's SIEFP and marks the SINT pending.
 *
 * @param[out] Newly
 * TRUE if the flag was previously clear.
 */
ULONG64
NTAPI
ReactVSynicSignalEvent(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Sint,
    _In_ ULONG Flag,
    _Out_ PBOOLEAN Newly)
{
    PREACTV_VP_SYNIC Synic = ReactVSynicOf(Vp);
    ULONG64 Where;
    ULONG Bank;
    ULONG Bit;

    *Newly = FALSE;

    if (!(Synic->Control & HV_SYNIC_CONTROL_ENABLE) ||
        !(Synic->EventPage & HV_SYNIC_PAGE_ENABLE))
    {
        return HV_STATUS_INVALID_SYNIC_STATE;
    }

    if (Synic->Sint[Sint] & HV_SYNIC_SINT_MASKED)
        return HV_STATUS_INVALID_SYNIC_STATE;

    /* Each SINT owns a 256 byte flag bitmap, accessed as 32 bit words */
    Where = (Synic->EventPage & HV_SYNIC_PAGE_MASK) +
            ((ULONG64)Sint * REACTV_SYNIC_MESSAGE_SIZE) +
            ((Flag / 32) * sizeof(ULONG));
    Bit = 1UL << (Flag % 32);

    if (!ReactVGuestRead(Partition, Where, &Bank, sizeof(Bank)))
        return HV_STATUS_INVALID_SYNIC_STATE;

    if (!(Bank & Bit))
    {
        Bank |= Bit;

        if (!ReactVGuestWrite(Partition, Where, &Bank, sizeof(Bank)))
            return HV_STATUS_INVALID_SYNIC_STATE;

        *Newly = TRUE;
    }

    Synic->Pending |= 1UL << Sint;

    return HV_STATUS_SUCCESS;
}

/* CHILD SYNTHETIC TIMERS *****************************************************/

/*
 * No timer interrupt in the core: expiry is checked on VM exits only. Timers
 * are never early and at most one exit gap late, hence the minimum period.
 */

/* SINT field of a timer config */
static
ULONG
NTAPI
ReactVTimerSint(
    _In_ ULONG64 Config)
{
    return (ULONG)((Config >> HV_STIMER_SINT_SHIFT) & HV_STIMER_SINT_MASK);
}

/* Next expiry for a timer that was just armed or just fired */
static
ULONG64
NTAPI
ReactVTimerNext(
    _In_ ULONG64 Config,
    _In_ ULONG64 Count,
    _In_ ULONG64 Now)
{
    if (!(Config & HV_STIMER_PERIODIC))
    {
        /* One shot COUNT is an absolute reference time */
        return Count;
    }

    if (Count < HV_STIMER_MINIMUM_PERIOD)
        return Now + HV_STIMER_MINIMUM_PERIOD;

    return Now + Count;
}

/* Arms or disarms a timer after a CONFIG or COUNT write */
static
VOID
NTAPI
ReactVTimerArm(
    _In_ PREACTV_VP_SYNIC Synic,
    _In_ ULONG Timer)
{
    ULONG64 Config = Synic->TimerConfig[Timer];

    if (!(Config & HV_STIMER_ENABLE) || (Synic->TimerCount[Timer] == 0))
    {
        Synic->TimerDue[Timer] = 0;
        return;
    }

    Synic->TimerDue[Timer] = ReactVTimerNext(Config,
                                             Synic->TimerCount[Timer],
                                             ReactVReferenceTime());
}

/**
 * @brief
 * Posts timer expired messages for a VP's expired synthetic timers.
 *
 * @remarks
 * A timer whose message cannot be posted stays due and is retried on a later exit.
 */
VOID
NTAPI
ReactVSynicTimers(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp)
{
    PREACTV_VP_SYNIC Synic = &Vp->Synic;
    HV_MESSAGE Message;
    PHV_TIMER_MESSAGE_PAYLOAD Payload;
    ULONG64 Now;
    ULONG Timer;

    if (!(Synic->Control & HV_SYNIC_CONTROL_ENABLE))
        return;

    Now = ReactVReferenceTime();

    for (Timer = 0; Timer < HV_SYNIC_STIMER_COUNT; Timer++)
    {
        ULONG64 Due = Synic->TimerDue[Timer];
        ULONG Sint;

        if ((Due == 0) || (Due > Now))
            continue;

        Sint = ReactVTimerSint(Synic->TimerConfig[Timer]);

        RtlZeroMemory(&Message, sizeof(Message));

        Message.MessageType = HvMessageTypeTimerExpired;
        Message.PayloadSize = sizeof(*Payload);

        Payload = (PHV_TIMER_MESSAGE_PAYLOAD)Message.Payload;
        Payload->TimerIndex = Timer;
        Payload->ExpirationTime = Due;
        Payload->DeliveryTime = Now;

        if (ReactVSynicPostMessage(Partition, Vp, Sint, &Message) != HV_STATUS_SUCCESS)
            continue;

        /* Periodic: rearm from now. One shot: disarm, and clear Enable if AutoEnable */
        if (Synic->TimerConfig[Timer] & HV_STIMER_PERIODIC)
        {
            Synic->TimerDue[Timer] = ReactVTimerNext(Synic->TimerConfig[Timer],
                                                     Synic->TimerCount[Timer],
                                                     Now);
            continue;
        }

        Synic->TimerDue[Timer] = 0;

        if (Synic->TimerConfig[Timer] & HV_STIMER_AUTO_ENABLE)
            Synic->TimerConfig[Timer] &= ~HV_STIMER_ENABLE;
    }
}

/* ROOT SYNIC *****************************************************************/

/*
 * Root SynIC. Same message/event semantics as a child, but the root has no
 * SLAT so its pages are written directly by the core.
 */

/* Pending root SINTs */
static ULONG ReactVRootPending;

/*
 * Overflow queue for SINT 0 messages while the root's slot is busy. Only the
 * core posts there, one at a time, so a few entries suffice. A dropped message
 * leaves a VP thread parked, so overflow is logged.
 */
#define REACTV_ROOT_QUEUE   4

static HV_MESSAGE ReactVRootQueue[REACTV_ROOT_QUEUE];
static ULONG ReactVRootQueued;

/*
 * Per SINT port ID ring in the SIRBP page. Byte 1 is the overflow flag and
 * entries start at offset 4. The root clears each entry as it consumes it, so
 * the core tracks the write index.
 */
#define REACTV_RING_ENTRIES     63
#define REACTV_RING_BEHIND      1
#define REACTV_RING_FIRST       4

static UCHAR ReactVRootRing[HV_SYNIC_SINT_COUNT];

/* Physical address of a root SINT's message slot, or zero if SIMP is disabled */
static
ULONG64
NTAPI
ReactVRootMessageSlot(
    _In_ ULONG Sint)
{
    if (!(ReactVSynic.MessagePage & HV_SYNIC_PAGE_ENABLE))
        return 0;

    return (ReactVSynic.MessagePage & HV_SYNIC_PAGE_MASK) +
           ((ULONG64)Sint * REACTV_SYNIC_MESSAGE_SIZE);
}

/**
 * @brief
 * Appends a port ID to the root's ring for a SINT.
 *
 * @return
 * FALSE if the ring is full; the overflow byte is set so the root rescans.
 */
static
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicRingRoot(
    _In_ ULONG Sint,
    _In_ ULONG PortId)
{
    PUCHAR Ring;
    PULONG Entries;
    UCHAR Next;

    if (!(ReactVSynic.RetargetPage & HV_SYNIC_PAGE_ENABLE))
    {
        ReactVLogWarning("Root SIRBP disabled, no port ring for SINT %lu\n", Sint);
        return FALSE;
    }

    Ring = ReactVRootPointer((ReactVSynic.RetargetPage & HV_SYNIC_PAGE_MASK) +
                                 ((ULONG64)Sint * REACTV_SYNIC_MESSAGE_SIZE),
                             REACTV_SYNIC_MESSAGE_SIZE);
    if (Ring == NULL)
        return FALSE;

    Entries = (PULONG)(Ring + REACTV_RING_FIRST);

    /* Entry not consumed yet */
    if (Entries[ReactVRootRing[Sint]] != 0)
    {
        Ring[REACTV_RING_BEHIND] = 1;
        return FALSE;
    }

    Entries[ReactVRootRing[Sint]] = PortId;

    Next = ReactVRootRing[Sint] + 1;
    ReactVRootRing[Sint] = (Next < REACTV_RING_ENTRIES) ? Next : 0;

    return TRUE;
}

/**
 * @brief
 * Posts a message to one of the root's SINTs.
 *
 * @param[in] PortId
 * Destination port, or zero for none. A nonzero port is also appended to the
 * SINT's SIRBP port ring.
 */
ULONG64
NTAPI
ReactVSynicPostToRoot(
    _In_ ULONG Sint,
    _In_ ULONG PortId,
    _In_ PHV_MESSAGE Message)
{
    ULONG64 Slot = ReactVRootMessageSlot(Sint);
    PHV_MESSAGE Where;

    if (!(ReactVSynic.Control & HV_SYNIC_CONTROL_ENABLE) || (Slot == 0))
    {
        ReactVLogWarning("Root SynIC or SIMP disabled, dropping message on SINT %lu\n",
                         Sint);
        return HV_STATUS_INVALID_SYNIC_STATE;
    }

    if (ReactVSynic.Sint[Sint] & HV_SYNIC_SINT_MASKED)
        return HV_STATUS_INVALID_SYNIC_STATE;

    Where = ReactVRootPointer(Slot, REACTV_SYNIC_MESSAGE_SIZE);
    if (Where == NULL)
        return HV_STATUS_INVALID_SYNIC_STATE;

    /* Slot busy: queue it and set MessagePending so the root writes EOM */
    if (Where->MessageType != 0)
    {
        /* Only SINT 0 is queued, since only the core posts there */
        if (Sint != HV_SYNIC_HYPERVISOR_SINT)
            return HV_STATUS_INSUFFICIENT_BUFFERS;

        if (ReactVRootQueued >= REACTV_ROOT_QUEUE)
        {
            ReactVLogWarning("Root SINT 0 queue full (%u messages)\n",
                             REACTV_ROOT_QUEUE);
            return HV_STATUS_INSUFFICIENT_BUFFERS;
        }

        ReactVRootQueue[ReactVRootQueued] = *Message;
        ReactVRootQueued++;

        Where->MessageFlags |= REACTV_MESSAGE_PENDING;

        return HV_STATUS_SUCCESS;
    }

    RtlCopyMemory(Where, Message, REACTV_SYNIC_MESSAGE_SIZE);

    ReactVLogTrace("Posted message type %lx to root SINT %lu\n",
                   Message->MessageType,
                   Sint);

    if ((PortId != 0) && !ReactVSynicRingRoot(Sint, PortId))
        return HV_STATUS_INSUFFICIENT_BUFFERS;

    return ReactVSynicRootWanted(Sint);
}

/**
 * @brief
 * Moves the next queued SINT 0 message into the root's slot on EOM.
 *
 * @remarks
 * The root clears MessageType before writing EOM, so the slot is already free.
 */
static
VOID
NTAPI
ReactVSynicRootFinished(VOID)
{
    ULONG64 Slot = ReactVRootMessageSlot(HV_SYNIC_HYPERVISOR_SINT);
    PHV_MESSAGE Where;
    ULONG Index;

    if ((Slot == 0) || (ReactVRootQueued == 0))
        return;

    Where = ReactVRootPointer(Slot, REACTV_SYNIC_MESSAGE_SIZE);
    if (Where == NULL)
        return;

    RtlCopyMemory(Where, &ReactVRootQueue[0], sizeof(*Where));

    for (Index = 1; Index < ReactVRootQueued; Index++)
        ReactVRootQueue[Index - 1] = ReactVRootQueue[Index];

    ReactVRootQueued--;

    if (ReactVRootQueued != 0)
        Where->MessageFlags |= REACTV_MESSAGE_PENDING;

    ReactVRootPending |= 1UL << HV_SYNIC_HYPERVISOR_SINT;
}

/**
 * @brief
 * Sends the root a signal bitset message marking a VP runnable.
 *
 * @remarks
 * This is the only wake for a root thread parked on a VP that returned blocked.
 */
VOID
NTAPI
ReactVSynicSignalRunnable(
    _In_ ULONG64 PartitionId,
    _In_ ULONG VpIndex)
{
    PREACTV_SIGNAL_BITSET Signal;
    HV_MESSAGE Message;

    RtlZeroMemory(&Message, sizeof(Message));

    Message.MessageType = ReactVMessageSignalBitset;
    Message.PayloadSize = sizeof(*Signal);

    Signal = (PREACTV_SIGNAL_BITSET)Message.Payload;
    Signal->PartitionId = PartitionId;
    Signal->BankMask = 1ULL << (VpIndex / 64);
    Signal->Bank[0] = 1ULL << (VpIndex % 64);

    ReactVSynicPostToRoot(HV_SYNIC_HYPERVISOR_SINT, 0, &Message);
}

/**
 * @brief
 * Raises a root SINT, or reports it through SINT 0 if it is proxied.
 */
static
ULONG64
NTAPI
ReactVSynicRootWanted(
    _In_ ULONG Sint)
{
    if ((Sint != HV_SYNIC_HYPERVISOR_SINT) &&
        (ReactVSynic.Sint[Sint] & HV_SYNIC_SINT_PROXY))
    {
        return ReactVSynicTellRoot(Sint);
    }

    ReactVRootPending |= 1UL << Sint;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Posts a SynIC event message on SINT 0 for a proxied root SINT.
 *
 * @remarks
 * A proxied SINT is signaled by a message on SINT 0 instead of an interrupt on
 * its own vector.
 */
static
ULONG64
NTAPI
ReactVSynicTellRoot(
    _In_ ULONG Sint)
{
    HV_MESSAGE Message;

    RtlZeroMemory(&Message, sizeof(Message));

    Message.MessageType = ReactVMessageSynicEvent;
    Message.PayloadSize = sizeof(ULONG64);
    Message.Payload[0] = (UCHAR)Sint;

    return ReactVSynicPostToRoot(HV_SYNIC_HYPERVISOR_SINT, 0, &Message);
}

/**
 * @brief
 * Sets an event flag in the root's SIEFP for a SINT.
 */
ULONG64
NTAPI
ReactVSynicSignalRoot(
    _In_ ULONG Sint,
    _In_ ULONG PortId,
    _In_ ULONG Flag,
    _Out_ PBOOLEAN Newly)
{
    PULONG Bank;
    ULONG64 Where;
    ULONG Bit;

    *Newly = FALSE;

    if (!(ReactVSynic.Control & HV_SYNIC_CONTROL_ENABLE) ||
        !(ReactVSynic.EventFlagsPage & HV_SYNIC_PAGE_ENABLE))
    {
        ReactVLogWarning("Root SynIC or SIEFP disabled, dropping event on SINT %lu\n",
                         Sint);
        return HV_STATUS_INVALID_SYNIC_STATE;
    }

    if (ReactVSynic.Sint[Sint] & HV_SYNIC_SINT_MASKED)
        return HV_STATUS_INVALID_SYNIC_STATE;

    Where = (ReactVSynic.EventFlagsPage & HV_SYNIC_PAGE_MASK) +
            ((ULONG64)Sint * REACTV_SYNIC_MESSAGE_SIZE) +
            ((Flag / 32) * sizeof(ULONG));

    Bank = ReactVRootPointer(Where, sizeof(*Bank));
    if (Bank == NULL)
        return HV_STATUS_INVALID_SYNIC_STATE;

    Bit = 1UL << (Flag % 32);

    if (!(*Bank & Bit))
    {
        *Bank |= Bit;
        *Newly = TRUE;
    }

    if ((PortId != 0) && !ReactVSynicRingRoot(Sint, PortId))
        return HV_STATUS_INSUFFICIENT_BUFFERS;

    return ReactVSynicRootWanted(Sint);
}

/**
 * @brief
 * Injects the lowest pending root SINT vector.
 *
 * @remarks
 * Called on VM entry to the root. If injection is blocked (IF clear, interrupt
 * shadow, or an event already queued), it is retried on the next entry.
 */
VOID
NTAPI
ReactVSynicDeliverToRoot(
    _In_ PREACTV_VMCB Vmcb)
{
    ULONG Sint;

    if (ReactVRootPending == 0)
        return;

    if (Vmcb->Control.EventInject != 0)
        return;

    if (!(Vmcb->Save.Rflags & AMD64_RFLAGS_IF) ||
        (Vmcb->Control.InterruptShadow & REACTV_SHADOW_INTERRUPT))
    {
        return;
    }

    for (Sint = 0; Sint < HV_SYNIC_SINT_COUNT; Sint++)
    {
        ULONG64 Setting = ReactVSynic.Sint[Sint];

        if (!(ReactVRootPending & (1UL << Sint)))
            continue;

        ReactVRootPending &= ~(1UL << Sint);

        if (Setting & HV_SYNIC_SINT_MASKED)
            continue;

        Vmcb->Control.EventInject = (Setting & HV_SYNIC_SINT_VECTOR_MASK) |
                                    ((ULONG64)REACTV_EVENT_TYPE_INTERRUPT <<
                                     REACTV_EVENT_TYPE_SHIFT) |
                                    REACTV_EVENT_VALID;
        Vmcb->Control.VmcbClean = 0;

        ReactVLogTrace("Injecting vector %lu for root SINT %lu\n",
                       (ULONG)(Setting & HV_SYNIC_SINT_VECTOR_MASK),
                       Sint);

        return;
    }
}

/* HYPERCALLS *****************************************************************/

/* Resolves and validates the target VP of a post message or signal event call */
static
PREACTV_VP
NTAPI
ReactVSynicTargetVp(
    _In_ PREACTV_HYPERCALL Call,
    _In_ ULONG64 PartitionId,
    _In_ ULONG VpIndex,
    _In_ ULONG Sint,
    _Out_ PREACTV_PARTITION *Partition)
{
    PREACTV_VP Vp;

    *Partition = ReactVPartitionFromCall(Call, PartitionId);
    if (*Partition == NULL)
        return NULL;

    if ((VpIndex >= REACTV_MAXIMUM_VPS) || (Sint >= HV_SYNIC_SINT_COUNT))
        return NULL;

    Vp = &(*Partition)->Vp[VpIndex];
    if (!Vp->Created)
        return NULL;

    return Vp;
}

static
ULONG64
NTAPI
ReactVPostMessageDirect(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_POST_MESSAGE_DIRECT Input;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Vp = ReactVSynicTargetVp(Call,
                             Input->PartitionId,
                             Input->VpIndex,
                             Input->SintIndex,
                             &Partition);
    if (Vp == NULL)
    {
        ReactVLogWarning("PostMessageDirect: bad target partition %I64x VP %lu SINT %lu\n",
                         Input->PartitionId,
                         Input->VpIndex,
                         Input->SintIndex);
        return HV_STATUS_INVALID_PARAMETER;
    }

    return ReactVSynicPostMessage(Partition,
                           Vp,
                           Input->SintIndex,
                           (PHV_MESSAGE)Input->Message);
}

static
ULONG64
NTAPI
ReactVSignalEventDirect(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_SIGNAL_EVENT_DIRECT Input;
    PHV_OUTPUT_SIGNAL_EVENT_DIRECT Output;
    PREACTV_PARTITION Partition;
    PREACTV_VP Vp;
    BOOLEAN Newly = FALSE;
    ULONG64 Status;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Vp = ReactVSynicTargetVp(Call,
                             Input->PartitionId,
                             Input->VpIndex,
                             Input->SintIndex,
                             &Partition);
    if (Vp == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Status = ReactVSynicSignalEvent(Partition,
                               Vp,
                               Input->SintIndex,
                               Input->FlagNumber,
                               &Newly);

    RtlZeroMemory(Output, sizeof(*Output));
    Output->NewlySignalled = Newly ? 1 : 0;

    return Status;
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicHypercall(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *Status,
    _Out_ ULONG64 *RepsCompleted)
{
    *RepsCompleted = 0;

    switch (Call->Code)
    {
        case HvCallPostMessageDirect:
            *Status = ReactVPostMessageDirect(Call);
            return TRUE;

        case HvCallSignalEventDirect:
            *Status = ReactVSignalEventDirect(Call);
            return TRUE;

        default:
            break;
    }

    return FALSE;
}

/* EOF */
