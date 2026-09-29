/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     AP startup trampoline and bring-up
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"
#include "reactv_svm.h"

/*
 * AP startup trampoline: real mode at the SIPI vector, to long mode, then into
 * the core. Hand encoded because the assembler only emits 64 bit code. Offsets
 * match the listing below.
 */

/* Trampoline page layout */
#define REACTV_AP_GDTR_OFFSET   0x60
#define REACTV_AP_GDT_OFFSET    0x68
#define REACTV_AP_LONG_OFFSET   0x80

/* Patched fields */
#define REACTV_AP_PATCH_CR3     0x13
#define REACTV_AP_PATCH_JUMP    0x38
#define REACTV_AP_PATCH_GDTBASE (REACTV_AP_GDTR_OFFSET + 2)
#define REACTV_AP_PATCH_STACK   0x8D
#define REACTV_AP_PATCH_INDEX   0x97
#define REACTV_AP_PATCH_ENTRY   0xA1

#define REACTV_AP_CODE_SELECTOR 0x08
#define REACTV_AP_DATA_SELECTOR 0x10

/* Spin count to wait for an AP to check in */
#define REACTV_AP_SPINS 0x2000000

/* GLOBALS ********************************************************************/

/*
 * 0000 fa                    cli
 * 0001 2e 0f 01 16 60 00     lgdt  cs:[0060]
 * 0007 0f 20 e0              mov   eax, cr4
 * 000a 66 83 c8 20           or    eax, 20        ; PAE
 * 000e 0f 22 e0              mov   cr4, eax
 * 0011 66 b8 ?? ?? ?? ??     mov   eax, <pml4>
 * 0017 0f 22 d8              mov   cr3, eax
 * 001a 66 b9 80 00 00 c0     mov   ecx, c0000080  ; EFER
 * 0020 0f 32                 rdmsr
 * 0022 66 0d 00 01 00 00     or    eax, 100       ; LME
 * 0028 0f 30                 wrmsr
 * 002a 0f 20 c0              mov   eax, cr0
 * 002d 66 0d 01 00 00 80     or    eax, 80000001  ; PG and PE together
 * 0033 0f 22 c0              mov   cr0, eax
 * 0036 66 ea ?? ?? ?? ?? 08  jmp   0008:<long>
 *
 * 0080 b8 10 00 00 00        mov   eax, 10
 * 0085 8e d8                 mov   ds, ax
 * 0087 8e c0                 mov   es, ax
 * 0089 8e d0                 mov   ss, ax
 * 008b 48 bc ?? x8           mov   rsp, <stack>
 * 0095 48 b9 ?? x8           mov   rcx, <index>
 * 009f 48 b8 ?? x8           mov   rax, <entry>
 * 00a9 ff e0                 jmp   rax
 */
static const UCHAR ReactVApTrampoline[] =
{
    /* 0000 */ 0xFA,
    /* 0001 */ 0x2E, 0x0F, 0x01, 0x16, REACTV_AP_GDTR_OFFSET, 0x00,
    /* 0007 */ 0x0F, 0x20, 0xE0,
    /* 000a */ 0x66, 0x83, 0xC8, 0x20,
    /* 000e */ 0x0F, 0x22, 0xE0,
    /* 0011 */ 0x66, 0xB8, 0x00, 0x00, 0x00, 0x00,
    /* 0017 */ 0x0F, 0x22, 0xD8,
    /* 001a */ 0x66, 0xB9, 0x80, 0x00, 0x00, 0xC0,
    /* 0020 */ 0x0F, 0x32,
    /* 0022 */ 0x66, 0x0D, 0x00, 0x01, 0x00, 0x00,
    /* 0028 */ 0x0F, 0x30,
    /* 002a */ 0x0F, 0x20, 0xC0,
    /* 002d */ 0x66, 0x0D, 0x01, 0x00, 0x00, 0x80,
    /* 0033 */ 0x0F, 0x22, 0xC0,
    /* 0036 */ 0x66, 0xEA, 0x00, 0x00, 0x00, 0x00, REACTV_AP_CODE_SELECTOR, 0x00
};

C_ASSERT(sizeof(ReactVApTrampoline) == 0x3E);

static const UCHAR ReactVApLongMode[] =
{
    /* 0080 */ 0xB8, REACTV_AP_DATA_SELECTOR, 0x00, 0x00, 0x00,
    /* 0085 */ 0x8E, 0xD8,
    /* 0087 */ 0x8E, 0xC0,
    /* 0089 */ 0x8E, 0xD0,
    /* 008b */ 0x48, 0xBC, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0095 */ 0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 009f */ 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 00a9 */ 0xFF, 0xE0
};

C_ASSERT(sizeof(ReactVApLongMode) == 0x2B);

/* Null, flat 64 bit code, flat data */
static const UCHAR ReactVApGdt[] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x9A, 0xA0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x92, 0xC0, 0x00
};

/* Set by the AP on entry, polled by the BSP */
static volatile LONG ReactVApArrived;

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
ReactVApPut32(
    _In_ PUCHAR Page,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    Page[Offset + 0] = (UCHAR)Value;
    Page[Offset + 1] = (UCHAR)(Value >> 8);
    Page[Offset + 2] = (UCHAR)(Value >> 16);
    Page[Offset + 3] = (UCHAR)(Value >> 24);
}

static
VOID
NTAPI
ReactVApPut64(
    _In_ PUCHAR Page,
    _In_ ULONG Offset,
    _In_ ULONG64 Value)
{
    ReactVApPut32(Page, Offset, (ULONG)Value);
    ReactVApPut32(Page, Offset + 4, (ULONG)(Value >> 32));
}

/**
 * @brief
 * Allocates and builds the AP startup page.
 *
 * @remarks
 * Must run before the root starts, since loader allocation services go away then.
 */
_Must_inspect_result_
NTSTATUS
NTAPI
ReactVApInitialize(VOID)
{
    PUCHAR Page;
    NTSTATUS Status;

    Status = ReactVCore.Loader->Memory->AllocateStartupPage(&ReactVCore.StartupPage);
    if (!NT_SUCCESS(Status))
    {
        ReactVLogWarning("AP startup page allocation failed, %lx\n",
                         Status);
        return Status;
    }

    /* Identity map the page in the core's tables, since the trampoline loads that CR3 */
    Status = ReactVMapCoreRange(ReactVCore.StartupPage,
                                ReactVCore.StartupPage,
                                AMD64_PAGE_SIZE);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVCore.Loader->Memory->MapPhysical(ReactVCore.StartupPage,
                                                    AMD64_PAGE_SIZE,
                                                    (PVOID *)&Page);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Page, AMD64_PAGE_SIZE);

    RtlCopyMemory(Page, ReactVApTrampoline, sizeof(ReactVApTrampoline));
    RtlCopyMemory(Page + REACTV_AP_LONG_OFFSET,
                  ReactVApLongMode,
                  sizeof(ReactVApLongMode));
    RtlCopyMemory(Page + REACTV_AP_GDT_OFFSET, ReactVApGdt, sizeof(ReactVApGdt));

    /* GDTR limit and physical base */
    Page[REACTV_AP_GDTR_OFFSET] = (UCHAR)(sizeof(ReactVApGdt) - 1);
    Page[REACTV_AP_GDTR_OFFSET + 1] = 0;
    ReactVApPut32(Page,
                  REACTV_AP_PATCH_GDTBASE,
                  (ULONG)(ReactVCore.StartupPage + REACTV_AP_GDT_OFFSET));

    ReactVApPut32(Page, REACTV_AP_PATCH_CR3, (ULONG)ReactVCore.Space.Pml4);
    ReactVApPut32(Page,
                  REACTV_AP_PATCH_JUMP,
                  (ULONG)(ReactVCore.StartupPage + REACTV_AP_LONG_OFFSET));
    ReactVApPut64(Page,
                  REACTV_AP_PATCH_ENTRY,
                  (ULONG64)(ULONG_PTR)ReactVApEnter);

    ReactVLogTrace("AP startup page %I64x, SIPI vector %02x\n",
                   ReactVCore.StartupPage,
                   (ULONG)(ReactVCore.StartupPage >> AMD64_PAGE_SHIFT));

    return STATUS_SUCCESS;
}

/* Patch per-AP stack and index; APs are started one at a time */
static
VOID
NTAPI
ReactVApAim(
    _In_ PREACTV_PROCESSOR Processor)
{
    PUCHAR Page = (PUCHAR)(ULONG_PTR)ReactVCore.StartupPage;

    ReactVApPut64(Page, REACTV_AP_PATCH_STACK, Processor->HostStackTop);
    ReactVApPut64(Page, REACTV_AP_PATCH_INDEX, Processor->Index);
}

/**
 * @brief
 * Sends INIT assert, INIT deassert, SIPI, SIPI.
 *
 * @remarks
 * x2APIC only; this is only reached from an intercepted x2APIC ICR write.
 */
static
VOID
NTAPI
ReactVApSendStartup(
    _In_ ULONG ApicId,
    _In_ ULONG Vector)
{
    ULONG64 Destination = (ULONG64)ApicId << 32;

    __writemsr(REACTV_X2APIC_ICR, Destination | REACTV_ICR_INIT_ASSERT);
    __writemsr(REACTV_X2APIC_ICR, Destination | REACTV_ICR_INIT_DEASSERT);
    __writemsr(REACTV_X2APIC_ICR, Destination | REACTV_ICR_STARTUP | Vector);
    __writemsr(REACTV_X2APIC_ICR, Destination | REACTV_ICR_STARTUP | Vector);
}

/**
 * @brief
 * Checks whether the AP with this APIC id is already running under the core.
 *
 * @remarks
 * The OS sends SIPI twice. Hardware ignores the second once the AP has left
 * wait-for-SIPI, so the core must drop it too.
 */
BOOLEAN
NTAPI
ReactVApIsLive(
    _In_ ULONG ApicId)
{
    ULONG Index;

    for (Index = 0; Index < ReactVCore.ProcessorCount; Index++)
    {
        if (ReactVCore.Processor[Index].ApicId == ApicId)
            return ReactVCore.Processor[Index].Live;
    }

    return FALSE;
}

/**
 * @brief
 * Starts an AP under the core and runs the root on it.
 *
 * @return
 * FALSE if the AP is unknown, already live, or never checked in.
 */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVApStart(
    _In_ ULONG ApicId,
    _In_ ULONG GuestVector)
{
    PREACTV_PROCESSOR Processor = NULL;
    ULONG Index, Spin;

    if (ReactVCore.StartupPage == 0)
        return FALSE;

    /* Find the processor by APIC id */
    for (Index = 0; Index < ReactVCore.ProcessorCount; Index++)
    {
        if (ReactVCore.Processor[Index].ApicId == ApicId)
        {
            Processor = &ReactVCore.Processor[Index];
            break;
        }
    }

    if ((Processor == NULL) || Processor->Live)
        return FALSE;

    /* The root's SIPI vector, used as the guest start point */
    Processor->StartupVector = GuestVector;

    ReactVApAim(Processor);

    ReactVApArrived = 0;

    ReactVApSendStartup(ApicId,
                        (ULONG)(ReactVCore.StartupPage >> AMD64_PAGE_SHIFT));

    for (Spin = 0; Spin < REACTV_AP_SPINS; Spin++)
    {
        if (ReactVApArrived != 0)
        {
            /*
             * Mark live here, not after VMRUN, so a repeated SIPI arriving
             * during guest setup does not reset an AP that is already running.
             */
            Processor->Live = TRUE;

            ReactVLogTrace("AP %lu started, root at %04x:0000\n",
                           Processor->Index,
                           GuestVector << 8);
            return TRUE;
        }
    }

    ReactVLogError("AP %lu did not check in\n", Processor->Index);
    return FALSE;
}

/**
 * @brief
 * AP entry from the trampoline, in long mode. Does not return.
 */
VOID
NTAPI
ReactVApEnter(
    _In_ ULONG64 Index)
{
    PREACTV_PROCESSOR Processor = &ReactVCore.Processor[Index];

    ReactVApArrived = 1;

    if (!NT_SUCCESS(ReactVSvmEnterAsProcessor(Processor)))
    {
        /* Park it */
        for (;;)
            __halt();
    }

    for (;;)
        __halt();
}

/* EOF */
