/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Virtualization backend detection and root context reporting
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"

/* FUNCTIONS ******************************************************************/

static
BOOLEAN
NTAPI
ReactVIsSvmUsable(VOID)
{
    ULONG Registers[4];
    ULONG64 VmCr;

    ReactVCore.Loader->System->Cpuid(AMD64_CPUID_EXTENDED_MAX, 0, Registers);
    if (Registers[0] < AMD64_CPUID_EXTENDED_FEATURES)
    {
        ReactVLogError("No extended CPUID leaves\n");
        return FALSE;
    }

    ReactVCore.Loader->System->Cpuid(AMD64_CPUID_EXTENDED_FEATURES, 0, Registers);
    if (!(Registers[2] & AMD64_EXT_FEATURE_SVM))
    {
        ReactVLogError("The processor does not support SVM\n");
        return FALSE;
    }

    /* Firmware may set VM_CR.SVMDIS and lock it until reset */
    VmCr = ReactVCore.Loader->System->ReadMsr(AMD64_MSR_VM_CR);
    if ((VmCr & AMD64_VM_CR_SVMDIS) && (VmCr & AMD64_VM_CR_LOCK))
    {
        ReactVLogError("SVM is off and locked by firmware, VM_CR %I64x\n", VmCr);
        return FALSE;
    }

    if (VmCr & AMD64_VM_CR_SVMDIS)
        ReactVLogWarning("SVM is off but not locked, VM_CR %I64x\n", VmCr);

    return TRUE;
}

/* IA32_FEATURE_CONTROL: only locked with VMX disabled is fatal, unlocked can still be set */
static
BOOLEAN
NTAPI
ReactVIsVmxUsable(VOID)
{
    ULONG Registers[4];
    ULONG64 Control;

    ReactVCore.Loader->System->Cpuid(AMD64_CPUID_FEATURES, 0, Registers);
    if (!(Registers[2] & AMD64_FEATURE_VMX))
    {
        ReactVLogError("The processor does not support VMX\n");
        return FALSE;
    }

    Control = ReactVCore.Loader->System->ReadMsr(AMD64_MSR_FEATURE_CONTROL);
    if ((Control & AMD64_FEATURE_CONTROL_LOCK) &&
        !(Control & AMD64_FEATURE_CONTROL_VMX))
    {
        ReactVLogError("VMX is off and locked by firmware, control %I64x\n", Control);
        return FALSE;
    }

    if (!(Control & AMD64_FEATURE_CONTROL_LOCK))
        ReactVLogWarning("IA32_FEATURE_CONTROL not locked\n");

    return TRUE;
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVDetectBackend(VOID)
{
    ULONG Registers[4];
    ULONG Ebx, Ecx, Edx;

    ReactVCore.Loader->System->Cpuid(AMD64_CPUID_VENDOR, 0, Registers);
    Ebx = Registers[1];
    Ecx = Registers[2];
    Edx = Registers[3];

    if ((Ebx == AMD64_VENDOR_INTEL_EBX) && (Edx == AMD64_VENDOR_INTEL_EDX) &&
        (Ecx == AMD64_VENDOR_INTEL_ECX))
    {
        if (!ReactVIsVmxUsable())
            return STATUS_NOT_SUPPORTED;

        ReactVCore.Backend = ReactVBackendVmx;
        return STATUS_SUCCESS;
    }

    if ((Ebx != AMD64_VENDOR_AMD_EBX) || (Edx != AMD64_VENDOR_AMD_EDX) ||
        (Ecx != AMD64_VENDOR_AMD_ECX))
    {
        ReactVLogError("Unknown processor vendor %lx %lx %lx\n", Ebx, Edx, Ecx);
        return STATUS_NOT_SUPPORTED;
    }

    if (!ReactVIsSvmUsable())
        return STATUS_NOT_SUPPORTED;

    ReactVCore.Loader->System->Cpuid(AMD64_CPUID_SVM_FEATURES, 0, Registers);
    ReactVCore.SvmRevision = Registers[0] & AMD64_SVM_REVISION_MASK;
    ReactVCore.SvmAsidCount = Registers[1];
    ReactVCore.SvmFeatures = Registers[3];
    ReactVCore.Backend = ReactVBackendSvm;

    ReactVLogTrace("SVM revision %lu, %lu ASIDs, features %lx\n",
                   ReactVCore.SvmRevision,
                   ReactVCore.SvmAsidCount,
                   ReactVCore.SvmFeatures);

    if (!(ReactVCore.SvmFeatures & AMD64_SVM_FEATURE_NP))
    {
        ReactVLogError("The processor has no nested paging\n");
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}

VOID
NTAPI
ReactVReportRootContext(
    _In_ PREACTV_LAUNCH_CONTEXT Context)
{
    ReactVLogTrace("Root context %lu bytes, flags %lx\n",
                   Context->Size,
                   Context->Flags);
    ReactVLogTrace("  GDT %I64x limit %x, IDT %I64x limit %x\n",
                   Context->Gdtr.Base,
                   Context->Gdtr.Limit,
                   Context->Idtr.Base,
                   Context->Idtr.Limit);
    ReactVLogTrace("  CS %x SS %x DS %x ES %x FS %x GS %x TR %x LDTR %x\n",
                   Context->Segments.Cs,
                   Context->Segments.Ss,
                   Context->Segments.Ds,
                   Context->Segments.Es,
                   Context->Segments.Fs,
                   Context->Segments.Gs,
                   Context->Segments.Tr,
                   Context->Segments.Ldtr);
    ReactVLogTrace("  CR0 %I64x CR3 %I64x CR4 %I64x\n",
                   Context->Resume.Cr0,
                   Context->Resume.Cr3,
                   Context->Resume.Cr4);
    ReactVLogTrace("  EFER %I64x PAT %I64x XCR0 %I64x\n",
                   Context->Resume.Efer,
                   Context->Resume.Pat,
                   Context->XCr0);
    ReactVLogTrace("  FS base %I64x GS base %I64x kernel GS base %I64x\n",
                   Context->FsBase,
                   Context->GsBase,
                   Context->KernelGsBase);
    ReactVLogTrace("  RFLAGS %I64x\n", Context->Resume.Rflags);
}
