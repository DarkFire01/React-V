/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VMX enable and root partition launch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcs.h"

/* Intel counterpart of svm.c. VMCS fields go through vmwrite, and exits land at HOST_RIP */

/* VMX MSR bitmap is 4 KB (the SVM MSRPM is 8 KB) */
#define REACTV_VMX_MSR_BITMAP_PAGES 1

#define REACTV_HOST_STACK_PAGES 4

#define REACTV_RFLAGS_IF 0x0000000000000200ULL

/* Host selectors must have RPL 0 and TI 0 */
#define REACTV_SELECTOR_MASK 0xFFF8

/* Descriptor S bit, Intel SDM volume 3 chapter 3 */
#define REACTV_DESCRIPTOR_SYSTEM (1ULL << 44)

/* FUNCTIONS ******************************************************************/

/* Capability MSR low half is the must be 1 mask, high half the may be 1 mask */
static
ULONG
NTAPI
ReactVAdjustControls(
    _In_ ULONG Wanted,
    _In_ ULONG Capability)
{
    ULONG64 Allowed = __readmsr(Capability);

    Wanted |= (ULONG)Allowed;
    Wanted &= (ULONG)(Allowed >> 32);

    return Wanted;
}

/* Decodes a GDT descriptor into VMCS base, limit and access rights format */
static
VOID
NTAPI
ReactVReadSegment(
    _In_ PREACTV_LAUNCH_CONTEXT Context,
    _In_ USHORT Selector,
    _Out_ PULONG64 Base,
    _Out_ PULONG Limit,
    _Out_ PULONG Access)
{
    ULONG64 Descriptor;
    ULONG Offset;

    *Base = 0;
    *Limit = 0;

    /* Null or out of range selector is marked unusable */
    Offset = Selector & ~0x7;
    if ((Offset == 0) || ((Offset + sizeof(Descriptor)) > ((ULONG)Context->Gdtr.Limit + 1)))
    {
        *Access = REACTV_VMX_SEGMENT_UNUSABLE;
        return;
    }

    Descriptor = *(PULONG64)(ULONG_PTR)(Context->Gdtr.Base + Offset);

    *Limit = (ULONG)(Descriptor & 0xFFFF) |
             (ULONG)(((Descriptor >> 48) & 0xF) << 16);

    *Base = ((Descriptor >> 16) & 0xFFFFFF) |
            (((Descriptor >> 56) & 0xFF) << 24);

    /* Access byte (bit 40) goes to bits 0-7, flags nibble (bit 52) to bits 12-15 */
    *Access = (ULONG)(((Descriptor >> 40) & 0xFF) |
                      (((Descriptor >> 52) & 0xF) << 12));

    /* System descriptors are 16 bytes and carry base bits 32-63 */
    if (!(Descriptor & REACTV_DESCRIPTOR_SYSTEM) &&
        ((Offset + (2 * sizeof(Descriptor))) <= ((ULONG)Context->Gdtr.Limit + 1)))
    {
        ULONG64 High = *(PULONG64)(ULONG_PTR)(Context->Gdtr.Base + Offset + sizeof(Descriptor));

        *Base |= (High & 0xFFFFFFFF) << 32;
    }
}

static
BOOLEAN
NTAPI
ReactVWriteSegment(
    _In_ PREACTV_LAUNCH_CONTEXT Context,
    _In_ USHORT Selector,
    _In_ ULONG SelectorField,
    _In_ ULONG BaseField,
    _In_ ULONG LimitField,
    _In_ ULONG AccessField)
{
    ULONG64 Base;
    ULONG Limit, Access;

    ReactVReadSegment(Context, Selector, &Base, &Limit, &Access);

    return ReactVVmxWrite(SelectorField, Selector) &&
           ReactVVmxWrite(BaseField, Base) &&
           ReactVVmxWrite(LimitField, Limit) &&
           ReactVVmxWrite(AccessField, Access);
}

/* Guest state from the loader's launch context; unset fields stay zero */
static
BOOLEAN
NTAPI
ReactVWriteGuestState(
    _In_ PREACTV_LAUNCH_CONTEXT Context)
{
    BOOLEAN Ok = TRUE;

    Ok = Ok && ReactVWriteSegment(Context, Context->Segments.Es,
                                  REACTV_VMCS_GUEST_ES_SELECTOR,
                                  REACTV_VMCS_GUEST_ES_BASE,
                                  REACTV_VMCS_GUEST_ES_LIMIT,
                                  REACTV_VMCS_GUEST_ES_ACCESS);
    Ok = Ok && ReactVWriteSegment(Context, Context->Segments.Cs,
                                  REACTV_VMCS_GUEST_CS_SELECTOR,
                                  REACTV_VMCS_GUEST_CS_BASE,
                                  REACTV_VMCS_GUEST_CS_LIMIT,
                                  REACTV_VMCS_GUEST_CS_ACCESS);
    Ok = Ok && ReactVWriteSegment(Context, Context->Segments.Ss,
                                  REACTV_VMCS_GUEST_SS_SELECTOR,
                                  REACTV_VMCS_GUEST_SS_BASE,
                                  REACTV_VMCS_GUEST_SS_LIMIT,
                                  REACTV_VMCS_GUEST_SS_ACCESS);
    Ok = Ok && ReactVWriteSegment(Context, Context->Segments.Ds,
                                  REACTV_VMCS_GUEST_DS_SELECTOR,
                                  REACTV_VMCS_GUEST_DS_BASE,
                                  REACTV_VMCS_GUEST_DS_LIMIT,
                                  REACTV_VMCS_GUEST_DS_ACCESS);
    Ok = Ok && ReactVWriteSegment(Context, Context->Segments.Fs,
                                  REACTV_VMCS_GUEST_FS_SELECTOR,
                                  REACTV_VMCS_GUEST_FS_BASE,
                                  REACTV_VMCS_GUEST_FS_LIMIT,
                                  REACTV_VMCS_GUEST_FS_ACCESS);
    Ok = Ok && ReactVWriteSegment(Context, Context->Segments.Gs,
                                  REACTV_VMCS_GUEST_GS_SELECTOR,
                                  REACTV_VMCS_GUEST_GS_BASE,
                                  REACTV_VMCS_GUEST_GS_LIMIT,
                                  REACTV_VMCS_GUEST_GS_ACCESS);
    Ok = Ok && ReactVWriteSegment(Context, Context->Segments.Ldtr,
                                  REACTV_VMCS_GUEST_LDTR_SELECTOR,
                                  REACTV_VMCS_GUEST_LDTR_BASE,
                                  REACTV_VMCS_GUEST_LDTR_LIMIT,
                                  REACTV_VMCS_GUEST_LDTR_ACCESS);
    Ok = Ok && ReactVWriteSegment(Context, Context->Segments.Tr,
                                  REACTV_VMCS_GUEST_TR_SELECTOR,
                                  REACTV_VMCS_GUEST_TR_BASE,
                                  REACTV_VMCS_GUEST_TR_LIMIT,
                                  REACTV_VMCS_GUEST_TR_ACCESS);

    /* Long mode FS/GS bases come from MSRs, not descriptors */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_FS_BASE, Context->FsBase);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_GS_BASE, Context->GsBase);

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_GDTR_BASE, Context->Gdtr.Base);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_GDTR_LIMIT, Context->Gdtr.Limit);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_IDTR_BASE, Context->Idtr.Base);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_IDTR_LIMIT, Context->Idtr.Limit);

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_CR0, Context->Resume.Cr0);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_CR3, Context->Resume.Cr3);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_CR4, Context->Resume.Cr4);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_DR7, __readdr(7));

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_IA32_EFER, Context->Resume.Efer);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_IA32_PAT, Context->Resume.Pat);

    /* GUEST_RSP is written by the entry stub */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_RIP,
                              (ULONG64)(ULONG_PTR)ReactVVmxGuestResume);

    /* Root resumes with IF clear; the shim reenables interrupts */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_RFLAGS,
                              Context->Resume.Rflags & ~REACTV_RFLAGS_IF);

    /* No interrupt blocking, active state */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_INTERRUPTIBILITY, 0);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_GUEST_ACTIVITY, REACTV_VMX_ACTIVITY_ACTIVE);

    /* No shadow VMCS */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_LINK_POINTER, ~0ULL);

    return Ok;
}

/* Host state uses the core CR3 and stack; only the GDT/IDT are shared with the loader */
static
BOOLEAN
NTAPI
ReactVWriteHostState(
    _In_ PREACTV_LAUNCH_CONTEXT Context,
    _In_ PREACTV_PROCESSOR Processor)
{
    BOOLEAN Ok = TRUE;
    ULONG64 Base;
    ULONG Limit, Access;

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_CR0, __readcr0());
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_CR3, ReactVCore.Space.Pml4);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_CR4, __readcr4());

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_ES_SELECTOR,
                              Context->Segments.Es & REACTV_SELECTOR_MASK);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_CS_SELECTOR,
                              Context->Segments.Cs & REACTV_SELECTOR_MASK);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_SS_SELECTOR,
                              Context->Segments.Ss & REACTV_SELECTOR_MASK);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_DS_SELECTOR,
                              Context->Segments.Ds & REACTV_SELECTOR_MASK);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_FS_SELECTOR,
                              Context->Segments.Fs & REACTV_SELECTOR_MASK);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_GS_SELECTOR,
                              Context->Segments.Gs & REACTV_SELECTOR_MASK);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_TR_SELECTOR,
                              Context->Segments.Tr & REACTV_SELECTOR_MASK);

    /* HOST_TR_BASE must point at a valid TSS */
    ReactVReadSegment(Context, Context->Segments.Tr, &Base, &Limit, &Access);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_TR_BASE, Base);

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_FS_BASE, Context->FsBase);
    /* Per processor block, found through GS on every exit */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_GS_BASE, (ULONG64)(ULONG_PTR)Processor);

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_GDTR_BASE, Context->Gdtr.Base);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_IDTR_BASE, Context->Idtr.Base);

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_IA32_EFER, __readmsr(AMD64_MSR_EFER));
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_IA32_PAT, Context->Resume.Pat);

    /* HOST_RSP is written by the entry stub */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_HOST_RIP,
                              (ULONG64)(ULONG_PTR)ReactVVmxExitEntry);

    return Ok;
}

/*
 * CPUID and VMCALL exit unconditionally. The MSR bitmap is clear, and MSRs
 * outside its ranges (the synthetic range) always exit.
 */
static
BOOLEAN
NTAPI
ReactVWriteControls(VOID)
{
    BOOLEAN Ok = TRUE;
    ULONG64 Basic;
    ULONG PinMsr, ProcMsr, ExitMsr, EntryMsr;

    Basic = __readmsr(AMD64_MSR_VMX_BASIC);
    if (Basic & AMD64_VMX_BASIC_TRUE_CONTROLS)
    {
        PinMsr = AMD64_MSR_VMX_TRUE_PINBASED;
        ProcMsr = AMD64_MSR_VMX_TRUE_PROCBASED;
        ExitMsr = AMD64_MSR_VMX_TRUE_EXIT;
        EntryMsr = AMD64_MSR_VMX_TRUE_ENTRY;
    }
    else
    {
        PinMsr = AMD64_MSR_VMX_PINBASED_CTLS;
        ProcMsr = AMD64_MSR_VMX_PROCBASED_CTLS;
        ExitMsr = AMD64_MSR_VMX_EXIT_CTLS;
        EntryMsr = AMD64_MSR_VMX_ENTRY_CTLS;
    }

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_PINBASED_CONTROLS,
                              ReactVAdjustControls(0, PinMsr));

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_PROCBASED_CONTROLS,
                              ReactVAdjustControls(REACTV_PROC_USE_MSR_BITMAPS, ProcMsr));

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_MSR_BITMAP, ReactVCore.VmxMsrBitmap);

    /* 64-bit host, EFER saved and loaded on both sides */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_EXIT_CONTROLS,
                              ReactVAdjustControls(REACTV_EXIT_HOST_ADDRESS_SPACE |
                                                   REACTV_EXIT_SAVE_IA32_EFER |
                                                   REACTV_EXIT_LOAD_IA32_EFER,
                                                   ExitMsr));

    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_ENTRY_CONTROLS,
                              ReactVAdjustControls(REACTV_ENTRY_IA32E_MODE_GUEST |
                                                   REACTV_ENTRY_LOAD_IA32_EFER,
                                                   EntryMsr));

    /* No event injection, no exception exits, no MSR load/store lists */
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_ENTRY_INTERRUPT_INFO, 0);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_EXCEPTION_BITMAP, 0);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_EXIT_MSR_STORE_COUNT, 0);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_EXIT_MSR_LOAD_COUNT, 0);
    Ok = Ok && ReactVVmxWrite(REACTV_VMCS_ENTRY_MSR_LOAD_COUNT, 0);

    return Ok;
}

/* Applies the CR0/CR4 FIXED0 (must be 1) and FIXED1 (may be 1) masks, and sets CR4.VMXE */
static
VOID
NTAPI
ReactVApplyFixedBits(VOID)
{
    ULONG64 Value;

    Value = __readcr0();
    Value |= __readmsr(AMD64_MSR_VMX_CR0_FIXED0);
    Value &= __readmsr(AMD64_MSR_VMX_CR0_FIXED1);
    __writecr0(Value);

    Value = __readcr4() | AMD64_CR4_VMXE;
    Value |= __readmsr(AMD64_MSR_VMX_CR4_FIXED0);
    Value &= __readmsr(AMD64_MSR_VMX_CR4_FIXED1);
    __writecr4(Value);
}

static
NTSTATUS
NTAPI
ReactVEnableVmx(
    _In_ PREACTV_PROCESSOR Processor)
{
    ULONG64 Control;
    PULONG Revision;
    NTSTATUS Status;

    /* Lock with VMX enabled if firmware left it unlocked; locked without VMX is fatal */
    Control = __readmsr(AMD64_MSR_FEATURE_CONTROL);
    if (!(Control & AMD64_FEATURE_CONTROL_LOCK))
    {
        __writemsr(AMD64_MSR_FEATURE_CONTROL,
                   Control | AMD64_FEATURE_CONTROL_LOCK | AMD64_FEATURE_CONTROL_VMX);

        Control = __readmsr(AMD64_MSR_FEATURE_CONTROL);
    }

    if (!(Control & AMD64_FEATURE_CONTROL_VMX))
    {
        ReactVLogError("VMX is off and locked by firmware, control %I64x\n", Control);
        return STATUS_NOT_SUPPORTED;
    }

    ReactVApplyFixedBits();

    /* VMXON region starts with the VMCS revision id from VMX_BASIC */
    Status = ReactVCore.Loader->Memory->MapPhysical(Processor->VmxOnRegion,
                                                    AMD64_PAGE_SIZE,
                                                    (PVOID *)&Revision);
    if (!NT_SUCCESS(Status))
        return Status;

    *Revision = (ULONG)(__readmsr(AMD64_MSR_VMX_BASIC) & AMD64_VMX_BASIC_REVISION_MASK);

    if (!ReactVVmxOn(Processor->VmxOnRegion))
    {
        ReactVLogError("VMXON failed, region %I64x\n", Processor->VmxOnRegion);
        return STATUS_NOT_SUPPORTED;
    }

    ReactVLogTrace("VMX on, region %I64x, revision %lx\n",
                   Processor->VmxOnRegion,
                   *Revision);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
ReactVAllocateProcessorPages(
    _In_ PREACTV_PROCESSOR Processor)
{
    ULONG64 StackBase;
    NTSTATUS Status;

    Status = ReactVAllocateMapped(1, 1, ReactVMemoryImage, &Processor->VmxOnRegion);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVAllocateMapped(1, 1, ReactVMemoryImage, &Processor->Vmcs);
    if (!NT_SUCCESS(Status))
        return Status;

    /* One MSR bitmap shared by all processors */
    if (ReactVCore.VmxMsrBitmap == 0)
    {
        Status = ReactVAllocateMapped(REACTV_VMX_MSR_BITMAP_PAGES, 1, ReactVMemoryImage,
                                      &ReactVCore.VmxMsrBitmap);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = ReactVAllocateMapped(REACTV_HOST_STACK_PAGES, 1, ReactVMemoryHeap,
                                  &StackBase);
    if (!NT_SUCCESS(Status))
        return Status;

    Processor->HostStackTop = StackBase + (REACTV_HOST_STACK_PAGES * AMD64_PAGE_SIZE);

    ReactVLogTrace("Processor %lu: VMXON %I64x, VMCS %I64x, host stack %I64x\n",
                   Processor->Index,
                   Processor->VmxOnRegion,
                   Processor->Vmcs,
                   Processor->HostStackTop);
    return STATUS_SUCCESS;
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVVmxLaunchRoot(VOID)
{
    /* BSP, the processor the loader is running on */
    PREACTV_PROCESSOR Processor = &ReactVCore.Processor[0];
    PULONG Revision;
    NTSTATUS Status;

    Status = ReactVAllocateProcessorPages(Processor);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVEnableVmx(Processor);
    if (!NT_SUCCESS(Status))
        return Status;

    /* VMCS needs the same revision id */
    Status = ReactVCore.Loader->Memory->MapPhysical(Processor->Vmcs,
                                                    AMD64_PAGE_SIZE,
                                                    (PVOID *)&Revision);
    if (!NT_SUCCESS(Status))
        return Status;

    *Revision = (ULONG)(__readmsr(AMD64_MSR_VMX_BASIC) & AMD64_VMX_BASIC_REVISION_MASK);

    if (!ReactVVmxClear(Processor->Vmcs) || !ReactVVmxPtrLoad(Processor->Vmcs))
    {
        ReactVLogError("VMCLEAR/VMPTRLD failed, VMCS %I64x\n", Processor->Vmcs);
        return STATUS_UNSUCCESSFUL;
    }

    if (!ReactVWriteControls() ||
        !ReactVWriteHostState(&ReactVCore.Root, Processor) ||
        !ReactVWriteGuestState(&ReactVCore.Root))
    {
        ReactVLogError("VMWRITE failed\n");
        return STATUS_UNSUCCESSFUL;
    }

    ReactVLogTrace("Root entry RIP %p, CS %x, CR3 %I64x\n",
                   ReactVVmxGuestResume,
                   ReactVCore.Root.Segments.Cs,
                   ReactVCore.Root.Resume.Cr3);

    ReactVCore.Loader->Launch->Quiesce();

    /* Loader interface is off limits from here */
    ReactVCore.Live = TRUE;
    Processor->Live = TRUE;

    /* Set last: GS base points at the per processor block on every exit */
    __writemsr(AMD64_MSR_GS_BASE, (ULONG64)(ULONG_PTR)Processor);

    if (ReactVVmxEnterGuest(Processor->Vmcs,
                            ReactVCore.Space.Pml4,
                            Processor->HostStackTop))
    {
        /* Running as the root guest */
        return STATUS_SUCCESS;
    }

    /* vmlaunch failed; read the VM instruction error before VMXOFF */
    ReactVLogError("VMLAUNCH failed, instruction error %I64x\n",
                   ReactVVmxRead(REACTV_VMCS_INSTRUCTION_ERROR));

    /* Restore the loader's GS base and turn VMX off */
    __writemsr(AMD64_MSR_GS_BASE, ReactVCore.Root.GsBase);

    Processor->Live = FALSE;
    ReactVCore.Live = FALSE;

    ReactVVmxOff();
    __writecr4(__readcr4() & ~AMD64_CR4_VMXE);

    return STATUS_UNSUCCESSFUL;
}
