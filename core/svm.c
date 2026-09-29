/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SVM enable and root partition launch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"
#include "reactv_vmcb.h"
#include "arch/amd64/svmasm.h"

/* svm.S writes this field by offset */
C_ASSERT(FIELD_OFFSET(REACTV_VMCB, Save.Rsp) == REACTV_VMCB_RSP_OFFSET);

#define REACTV_HOST_STACK_PAGES 4

/* IOPM is 12 KB, MSRPM is 8 KB; a clear bit means no intercept */
#define REACTV_IOPM_PAGES 4
#define REACTV_MSRPM_PAGES 2

#define REACTV_RFLAGS_IF 0x0000000000000200ULL

/* Required before fxsave or SSE instructions can be used */
#define REACTV_CR4_OSFXSR       0x0000000000000200ULL
#define REACTV_CR4_OSXMMEXCPT   0x0000000000000400ULL

/* Reset state and real mode segment attributes (present, DPL 0, accessed, no G/D) */
#define REACTV_CR0_AT_RESET     0x0000000060000010ULL
#define REACTV_RFLAGS_AT_RESET  0x0000000000000002ULL
#define REACTV_SEGMENT_REAL_CODE 0x009B
#define REACTV_SEGMENT_REAL_DATA 0x0093

/* Descriptor S bit, AMD64 APM volume 2 chapter 4 */
#define REACTV_DESCRIPTOR_SYSTEM (1ULL << 44)

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
ReactVFillSegment(
    _In_ PREACTV_LAUNCH_CONTEXT Context,
    _In_ USHORT Selector,
    _Out_ PREACTV_VMCB_SEGMENT Segment)
{
    ULONG64 Descriptor;
    ULONG Offset;

    RtlZeroMemory(Segment, sizeof(*Segment));
    Segment->Selector = Selector;

    /* Null or out of range selector gives a null segment */
    Offset = Selector & ~0x7;
    if ((Offset == 0) || ((Offset + sizeof(Descriptor)) > ((ULONG)Context->Gdtr.Limit + 1)))
        return;

    Descriptor = *(PULONG64)(ULONG_PTR)(Context->Gdtr.Base + Offset);

    /* VMCB attributes: access byte in bits 0-7, flags nibble in bits 8-11 */
    Segment->Attributes = (USHORT)(((Descriptor >> 40) & 0xFF) |
                                   (((Descriptor >> 52) & 0xF) << 8));
    Segment->Limit = (ULONG)(Descriptor & 0xFFFF) |
                     (ULONG)(((Descriptor >> 48) & 0xF) << 16);
    Segment->Base = ((Descriptor >> 16) & 0xFFFFFF) |
                    (((Descriptor >> 56) & 0xFF) << 24);

    /* System descriptors are 16 bytes and carry base bits 32-63 */
    if (!(Descriptor & REACTV_DESCRIPTOR_SYSTEM) &&
        ((Offset + (2 * sizeof(Descriptor))) <= ((ULONG)Context->Gdtr.Limit + 1)))
    {
        ULONG64 High = *(PULONG64)(ULONG_PTR)(Context->Gdtr.Base + Offset + sizeof(Descriptor));

        Segment->Base |= (High & 0xFFFFFFFF) << 32;
    }
}

static
VOID
NTAPI
ReactVBuildRootVmcb(
    _In_ PREACTV_VMCB Vmcb,
    _In_ PREACTV_LAUNCH_CONTEXT Context)
{
    RtlZeroMemory(Vmcb, sizeof(*Vmcb));

    /*
     * VMRUN intercept is mandatory. CPUID, MSR and VMMCALL form the hypervisor
     * interface. The MSRPM is mostly clear; synthetic MSRs lie outside its ranges
     * and always exit.
     */
    Vmcb->Control.Intercept1 = REACTV_INTERCEPT_CPUID | REACTV_INTERCEPT_MSR;
    Vmcb->Control.Intercept2 = REACTV_INTERCEPT_VMRUN | REACTV_INTERCEPT_VMMCALL;
    Vmcb->Control.IopmBasePa = ReactVCore.Iopm;
    Vmcb->Control.MsrpmBasePa = ReactVCore.Msrpm;
    Vmcb->Control.GuestAsid = 1;
    Vmcb->Control.TlbControl = REACTV_TLB_FLUSH_ALL;

    ReactVFillSegment(Context, Context->Segments.Es, &Vmcb->Save.Es);
    ReactVFillSegment(Context, Context->Segments.Cs, &Vmcb->Save.Cs);
    ReactVFillSegment(Context, Context->Segments.Ss, &Vmcb->Save.Ss);
    ReactVFillSegment(Context, Context->Segments.Ds, &Vmcb->Save.Ds);
    ReactVFillSegment(Context, Context->Segments.Fs, &Vmcb->Save.Fs);
    ReactVFillSegment(Context, Context->Segments.Gs, &Vmcb->Save.Gs);
    ReactVFillSegment(Context, Context->Segments.Tr, &Vmcb->Save.Tr);
    ReactVFillSegment(Context, Context->Segments.Ldtr, &Vmcb->Save.Ldtr);

    /* Long mode FS/GS bases come from MSRs, not descriptors */
    Vmcb->Save.Fs.Base = Context->FsBase;
    Vmcb->Save.Gs.Base = Context->GsBase;
    Vmcb->Save.KernelGsBase = Context->KernelGsBase;

    Vmcb->Save.Gdtr.Base = Context->Gdtr.Base;
    Vmcb->Save.Gdtr.Limit = Context->Gdtr.Limit;
    Vmcb->Save.Idtr.Base = Context->Idtr.Base;
    Vmcb->Save.Idtr.Limit = Context->Idtr.Limit;

    Vmcb->Save.Cr0 = Context->Resume.Cr0;
    Vmcb->Save.Cr3 = Context->Resume.Cr3;
    Vmcb->Save.Cr4 = Context->Resume.Cr4;
    Vmcb->Save.Cr2 = __readcr2();

    /* vmrun rejects a guest with EFER.SVME clear */
    Vmcb->Save.Efer = Context->Resume.Efer | AMD64_EFER_SVME;
    Vmcb->Save.GuestPat = Context->Resume.Pat;

    Vmcb->Save.Dr6 = __readdr(6);
    Vmcb->Save.Dr7 = __readdr(7);

    /* RSP is filled in by ReactVSvmEnterGuest */
    Vmcb->Save.Rip = (ULONG64)(ULONG_PTR)ReactVSvmGuestResume;

    /* Root resumes with IF clear; the shim reenables interrupts */
    Vmcb->Save.Rflags = Context->Resume.Rflags & ~REACTV_RFLAGS_IF;
    Vmcb->Save.Cpl = 0;
}

static
NTSTATUS
NTAPI
ReactVEnableSvm(
    _In_ PREACTV_PROCESSOR Processor)
{
    ULONG64 VmCr, Efer;

    /* Detection already rejected a locked VM_CR, so SVMDIS is clearable here */
    VmCr = __readmsr(AMD64_MSR_VM_CR);
    if (VmCr & AMD64_VM_CR_SVMDIS)
    {
        __writemsr(AMD64_MSR_VM_CR, VmCr & ~(ULONG64)AMD64_VM_CR_SVMDIS);

        VmCr = __readmsr(AMD64_MSR_VM_CR);
        if (VmCr & AMD64_VM_CR_SVMDIS)
        {
            ReactVLogError("VM_CR.SVMDIS stuck set, VM_CR %I64x\n", VmCr);
            return STATUS_NOT_SUPPORTED;
        }
    }

    __writemsr(AMD64_MSR_VM_HSAVE_PA, Processor->HostSaveArea);

    Efer = __readmsr(AMD64_MSR_EFER);
    __writemsr(AMD64_MSR_EFER, Efer | AMD64_EFER_SVME);

    Efer = __readmsr(AMD64_MSR_EFER);
    if (!(Efer & AMD64_EFER_SVME))
    {
        ReactVLogError("EFER.SVME stuck clear, EFER %I64x\n", Efer);
        return STATUS_NOT_SUPPORTED;
    }

    ReactVLogTrace("SVM on, host save area %I64x\n", Processor->HostSaveArea);
    return STATUS_SUCCESS;
}

/* Allocates pages filled with one byte value. Child IOPM/MSRPM are all ones so every access exits */
static
NTSTATUS
NTAPI
ReactVAllocateFilled(
    _In_ ULONG64 PageCount,
    _In_ UCHAR Value,
    _Out_ ULONG64 *PhysicalBase)
{
    ULONG64 Base;
    PVOID Virtual;
    NTSTATUS Status;

    Status = ReactVAllocateMapped(PageCount, 1, ReactVMemoryImage, &Base);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVCore.Loader->Memory->MapPhysical(Base,
                                                    PageCount * AMD64_PAGE_SIZE,
                                                    &Virtual);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlFillMemory(Virtual, (SIZE_T)(PageCount * AMD64_PAGE_SIZE), Value);

    *PhysicalBase = Base;
    return STATUS_SUCCESS;
}

/* Permission maps are read only and shared by all processors */
static
NTSTATUS
NTAPI
ReactVAllocateSharedPages(VOID)
{
    PUCHAR Map;
    NTSTATUS Status;

    Status = ReactVAllocateMapped(REACTV_IOPM_PAGES, 1, ReactVMemoryImage,
                                  &ReactVCore.Iopm);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVAllocateMapped(REACTV_MSRPM_PAGES, 1, ReactVMemoryImage,
                                  &ReactVCore.Msrpm);
    if (!NT_SUCCESS(Status))
        return Status;

    /*
     * Intercept x2APIC ICR writes to catch every SIPI; an AP started outside the
     * core faults on its first hypercall. Two bits per MSR, read then write, so
     * the write bit for MSR n is bit 1 of byte n / 4.
     */
    Status = ReactVCore.Loader->Memory->MapPhysical(ReactVCore.Msrpm,
                                                    REACTV_MSRPM_PAGES * AMD64_PAGE_SIZE,
                                                    (PVOID *)&Map);
    if (!NT_SUCCESS(Status))
        return Status;

    Map[REACTV_X2APIC_ICR / 4] |= 2;

    Status = ReactVAllocateFilled(REACTV_IOPM_PAGES, 0xFF, &ReactVCore.ChildIopm);
    if (!NT_SUCCESS(Status))
        return Status;

    return ReactVAllocateFilled(REACTV_MSRPM_PAGES, 0xFF, &ReactVCore.ChildMsrpm);
}

/* Per processor host save area, VMCBs, FPU area and host stack */
static
NTSTATUS
NTAPI
ReactVAllocateProcessorPages(
    _In_ PREACTV_PROCESSOR Processor)
{
    ULONG64 StackBase;
    NTSTATUS Status;

    Status = ReactVAllocateMapped(1, 1, ReactVMemoryImage, &Processor->HostSaveArea);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVAllocateMapped(1, 1, ReactVMemoryImage, &Processor->Vmcb);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVAllocateMapped(1, 1, ReactVMemoryImage, &Processor->HostVmcb);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVAllocateMapped(1, 1, ReactVMemoryImage, &Processor->HostFpuState);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReactVAllocateMapped(REACTV_HOST_STACK_PAGES, 1, ReactVMemoryHeap,
                                  &StackBase);
    if (!NT_SUCCESS(Status))
        return Status;

    Processor->HostStackTop = StackBase + (REACTV_HOST_STACK_PAGES * AMD64_PAGE_SIZE);

    ReactVLogTrace("Processor %lu: VMCB %I64x, save area %I64x, host stack %I64x\n",
                   Processor->Index,
                   Processor->Vmcb,
                   Processor->HostSaveArea,
                   Processor->HostStackTop);
    return STATUS_SUCCESS;
}

/* Post SIPI state: real mode at CS = vector << 8, IP 0, as the root's AP trampoline expects */
static
VOID
NTAPI
ReactVBuildStartupVmcb(
    _In_ PREACTV_VMCB Vmcb,
    _In_ ULONG Vector)
{
    RtlZeroMemory(Vmcb, sizeof(*Vmcb));

    Vmcb->Control.Intercept1 = REACTV_INTERCEPT_CPUID | REACTV_INTERCEPT_MSR;
    Vmcb->Control.Intercept2 = REACTV_INTERCEPT_VMRUN | REACTV_INTERCEPT_VMMCALL;
    Vmcb->Control.IopmBasePa = ReactVCore.Iopm;
    Vmcb->Control.MsrpmBasePa = ReactVCore.Msrpm;
    Vmcb->Control.GuestAsid = 1;
    Vmcb->Control.TlbControl = REACTV_TLB_FLUSH_ALL;

    /* CS from the SIPI vector page, other segments based at zero */
    Vmcb->Save.Cs.Selector = (USHORT)(Vector << 8);
    Vmcb->Save.Cs.Base = (ULONG64)Vector << AMD64_PAGE_SHIFT;
    Vmcb->Save.Cs.Limit = 0xFFFF;
    Vmcb->Save.Cs.Attributes = REACTV_SEGMENT_REAL_CODE;

    Vmcb->Save.Ds.Limit = 0xFFFF;
    Vmcb->Save.Ds.Attributes = REACTV_SEGMENT_REAL_DATA;
    Vmcb->Save.Es = Vmcb->Save.Ds;
    Vmcb->Save.Ss = Vmcb->Save.Ds;
    Vmcb->Save.Fs = Vmcb->Save.Ds;
    Vmcb->Save.Gs = Vmcb->Save.Ds;

    Vmcb->Save.Gdtr.Limit = 0xFFFF;
    Vmcb->Save.Idtr.Limit = 0xFFFF;

    /* Reset values */
    Vmcb->Save.Cr0 = REACTV_CR0_AT_RESET;
    Vmcb->Save.Efer = AMD64_EFER_SVME;
    Vmcb->Save.Rflags = REACTV_RFLAGS_AT_RESET;
    Vmcb->Save.Rip = 0;
    Vmcb->Save.Rsp = 0;
    Vmcb->Save.Cpl = 0;
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVSvmEnterAsProcessor(
    _In_ PREACTV_PROCESSOR Processor)
{
    /* Core memory is identity mapped */
    PREACTV_VMCB Vmcb = (PREACTV_VMCB)(ULONG_PTR)Processor->Vmcb;
    REACTV_DESCRIPTOR_REGISTER Gdtr, Idtr;
    NTSTATUS Status;

    /* Switch from the startup page's minimal GDT (no IDT) to the root's GDT/IDT */
    Gdtr.Limit = ReactVCore.Root.Gdtr.Limit;
    Gdtr.Base = ReactVCore.Root.Gdtr.Base;
    Idtr.Limit = ReactVCore.Root.Idtr.Limit;
    Idtr.Base = ReactVCore.Root.Idtr.Base;

    ReactVSvmAdoptDescriptors(&Gdtr,
                              &Idtr,
                              ReactVCore.Root.Segments.Cs,
                              ReactVCore.Root.Segments.Ss);

    /*
     * fxsave and the XMM hypercall path need OSFXSR/OSXMMEXCPT, which the startup
     * page does not set. The BSP inherits them from the root's CR4.
     */
    __writecr4(__readcr4() | REACTV_CR4_OSFXSR | REACTV_CR4_OSXMMEXCPT);

    Status = ReactVEnableSvm(Processor);
    if (!NT_SUCCESS(Status))
        return Status;

    ReactVBuildStartupVmcb(Vmcb, Processor->StartupVector);

    Processor->Live = TRUE;

    __writemsr(AMD64_MSR_GS_BASE, (ULONG64)(ULONG_PTR)Processor);

    if (ReactVSvmEnterGuestAp(Processor->Vmcb,
                              ReactVCore.Space.Pml4,
                              Processor->HostStackTop))
    {
        return STATUS_SUCCESS;
    }

    ReactVLogError("AP %lu VMRUN failed, exit code %I64x\n",
                   Processor->Index,
                   Vmcb->Control.ExitCode);

    Processor->Live = FALSE;
    return STATUS_UNSUCCESSFUL;
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVSvmLaunchRoot(VOID)
{
    /* BSP, the processor the loader is running on */
    PREACTV_PROCESSOR Processor = &ReactVCore.Processor[0];
    PREACTV_VMCB Vmcb;
    NTSTATUS Status;
    ULONG Index;

    Status = ReactVAllocateSharedPages();
    if (!NT_SUCCESS(Status))
        return Status;

    /* Allocate for all APs now; the loader allocator is gone once the root is a guest */
    for (Index = 0; Index < ReactVCore.ProcessorCount; Index++)
    {
        Status = ReactVAllocateProcessorPages(&ReactVCore.Processor[Index]);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Without a startup page only the BSP runs under the core */
    ReactVLogTrace("Allocating AP startup page\n");

    if (!NT_SUCCESS(ReactVApInitialize()))
        ReactVCore.StartupPage = 0;

    ReactVLogTrace("AP startup %s\n",
                   (ReactVCore.StartupPage != 0) ? "available" : "unavailable");

    Status = ReactVEnableSvm(Processor);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Last use of the loader mapping */
    Status = ReactVCore.Loader->Memory->MapPhysical(Processor->Vmcb,
                                                    AMD64_PAGE_SIZE,
                                                    (PVOID *)&Vmcb);
    if (!NT_SUCCESS(Status))
        return Status;

    ReactVBuildRootVmcb(Vmcb, &ReactVCore.Root);

    ReactVLogTrace("Root entry RIP %I64x, CS %x, CR3 %I64x\n",
                   Vmcb->Save.Rip,
                   Vmcb->Save.Cs.Selector,
                   Vmcb->Save.Cr3);

    ReactVCore.Loader->Launch->Quiesce();

    /* Loader interface is off limits from here */
    ReactVCore.Live = TRUE;
    Processor->Live = TRUE;

    /* Set last: GS base points at the per processor block on every exit */
    __writemsr(AMD64_MSR_GS_BASE, (ULONG64)(ULONG_PTR)Processor);

    if (ReactVSvmEnterGuest(Processor->Vmcb,
                            ReactVCore.Space.Pml4,
                            Processor->HostStackTop))
    {
        /* Running as the root guest */
        return STATUS_SUCCESS;
    }

    /* vmrun failed; exit code -1 is VMEXIT_INVALID (bad VMCB state) */
    ReactVLogError("VMRUN failed, exit code %I64x, info %I64x\n",
                   Vmcb->Control.ExitCode,
                   Vmcb->Control.ExitInfo1);

    /* Restore the loader's GS base and turn SVM off */
    __writemsr(AMD64_MSR_GS_BASE, ReactVCore.Root.GsBase);
    __writemsr(AMD64_MSR_EFER, __readmsr(AMD64_MSR_EFER) & ~AMD64_EFER_SVME);
    __writemsr(AMD64_MSR_VM_HSAVE_PA, 0);

    Processor->Live = FALSE;
    ReactVCore.Live = FALSE;

    return STATUS_UNSUCCESSFUL;
}
