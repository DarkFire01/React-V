/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Post-launch hypervisor detection probe
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

#include "reactv_tlfs.h"

#include <debug.h>
DBG_DEFAULT_CHANNEL(HYPERVISOR);

/*
 * Runs in the root after launch and follows the enlightened guest detection
 * sequence: vendor leaf, interface signature, guest OS ID, hypercall page, hypercall.
 */

/* Guest OS ID per TLFS, vendor 0 is unassigned */
#define REACTV_PROBE_GUEST_OS_ID 0x0001000000000000ULL

typedef ULONG64
(__cdecl *REACTV_HYPERCALL_ROUTINE)(
    _In_ ULONG64 Control,
    _In_ ULONG64 InputGpa,
    _In_ ULONG64 OutputGpa);

/* FUNCTIONS ******************************************************************/

VOID
NTAPI
ReactVProbe(VOID)
{
    int Registers[4];
    ULONG64 HypercallPage;
    ULONG64 Result;
    REACTV_HYPERCALL_ROUTINE Hypercall;
    CHAR Vendor[13];

    __cpuid(Registers, 1);
    if (!((ULONG)Registers[2] & HV_CPUID_HYPERVISOR_PRESENT))
    {
        RV_ERR("Hypervisor present bit clear\n");
        return;
    }

    __cpuid(Registers, HV_CPUID_FUNCTION_VENDOR_AND_MAX_FUNCTION);
    RtlCopyMemory(&Vendor[0], &Registers[1], sizeof(ULONG));
    RtlCopyMemory(&Vendor[4], &Registers[2], sizeof(ULONG));
    RtlCopyMemory(&Vendor[8], &Registers[3], sizeof(ULONG));
    Vendor[12] = ANSI_NULL;

    RV_TRACE("Vendor '%s', leaves up to %lx\n", Vendor, (ULONG)Registers[0]);

    __cpuid(Registers, HV_CPUID_FUNCTION_INTERFACE);
    if ((ULONG)Registers[0] != HV_INTERFACE_SIGNATURE)
    {
        RV_ERR("Interface signature is %lx, not Hv#1\n", (ULONG)Registers[0]);
        return;
    }

    __cpuid(Registers, HV_CPUID_FUNCTION_VERSION);
    RV_TRACE("Version %lu.%lu build %lu\n",
          (ULONG)Registers[1] >> 16,
          (ULONG)Registers[1] & 0xFFFF,
          (ULONG)Registers[0]);

    __cpuid(Registers, HV_CPUID_FUNCTION_FEATURES);
    RV_TRACE("Privileges %lx\n", (ULONG)Registers[0]);

    /* Hypercall MSR is only usable with a nonzero guest OS ID */
    __writemsr(HV_X64_MSR_GUEST_OS_ID, REACTV_PROBE_GUEST_OS_ID);

    /* freeldr is identity mapped, so the VA is also the GPA */
    HypercallPage = (ULONG64)(ULONG_PTR)MmAllocateMemoryWithType(MM_PAGE_SIZE,
                                                                 LoaderFirmwarePermanent);
    if (HypercallPage == 0)
    {
        RV_ERR("Hypercall page allocation failed\n");
        return;
    }

    __writemsr(HV_X64_MSR_HYPERCALL, HypercallPage | HV_X64_MSR_HYPERCALL_ENABLE);

    if (__readmsr(HV_X64_MSR_HYPERCALL) != (HypercallPage | HV_X64_MSR_HYPERCALL_ENABLE))
    {
        RV_ERR("Hypercall MSR readback mismatch\n");
        return;
    }

    RV_TRACE("Hypercall page at %I64x, first bytes %02x %02x %02x %02x\n",
          HypercallPage,
          ((PUCHAR)(ULONG_PTR)HypercallPage)[0],
          ((PUCHAR)(ULONG_PTR)HypercallPage)[1],
          ((PUCHAR)(ULONG_PTR)HypercallPage)[2],
          ((PUCHAR)(ULONG_PTR)HypercallPage)[3]);

    /* Call through the hypercall page stub */
    Hypercall = (REACTV_HYPERCALL_ROUTINE)(ULONG_PTR)HypercallPage;

    Result = Hypercall(HvCallNotifyLongSpinWait, 0, 0);
    RV_TRACE("HvCallNotifyLongSpinWait returned %I64x\n", Result);

    Result = Hypercall(0xFFFF, 0, 0);
    RV_TRACE("Invalid call code returned %I64x\n", Result);

    RV_TRACE("VP index %I64x\n", __readmsr(HV_X64_MSR_VP_INDEX));

    /* Clearing the guest OS ID also disables the hypercall page */
    __writemsr(HV_X64_MSR_GUEST_OS_ID, 0);
}
