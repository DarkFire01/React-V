/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     freeldr launch interface: context capture, quiesce and resume
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

#include <ndk/asm.h>
#include <internal/amd64/intrin_i.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(HYPERVISOR);

#define REACTV_CR4_OSXSAVE 0x00040000
#define REACTV_RFLAGS_IF   0x00000200

/* ReactVFldrReadSelectors writes eight words in REACTV_SEGMENT_STATE order */
C_ASSERT(sizeof(REACTV_SEGMENT_STATE) == 8 * sizeof(USHORT));

/* GLOBALS ********************************************************************/

static BOOLEAN ReactVInterruptsWereOn;

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
ReactVCaptureContext(
    _Out_ PREACTV_LAUNCH_CONTEXT Context)
{
    ULONG64 Cr4;

    RtlZeroMemory(Context, sizeof(*Context));
    Context->Size = sizeof(*Context);

    ReactVFldrReadSelectors((PUSHORT)&Context->Segments);
    __sgdt(&Context->Gdtr);
    __sidt(&Context->Idtr);

    Context->FsBase = __readmsr(AMD64_MSR_FS_BASE);
    Context->GsBase = __readmsr(AMD64_MSR_GS_BASE);
    Context->KernelGsBase = __readmsr(AMD64_MSR_KERNEL_GS_BASE);

    Cr4 = __readcr4();
    if (Cr4 & REACTV_CR4_OSXSAVE)
    {
        Context->XCr0 = _xgetbv(0);
        Context->Flags |= REACTV_LAUNCH_XSAVE_VALID;
    }

    Context->Resume.Cr0 = __readcr0();
    Context->Resume.Cr3 = __readcr3();
    Context->Resume.Cr4 = Cr4;
    Context->Resume.Efer = __readmsr(AMD64_MSR_EFER);
    Context->Resume.Pat = __readmsr(AMD64_MSR_PAT);
    Context->Resume.Rflags = __readeflags();
}

static
VOID
NTAPI
ReactVQuiesce(VOID)
{
    ReactVInterruptsWereOn = ((__readeflags() & REACTV_RFLAGS_IF) != 0);
    _disable();

    RV_TRACE("Entering core\n");
}

static
VOID
NTAPI
ReactVResumed(
    _In_ NTSTATUS Status)
{
    if (ReactVInterruptsWereOn)
    {
        ReactVInterruptsWereOn = FALSE;
        _enable();
    }

    RV_TRACE("Resumed in loader, status %lx\n", Status);
}

REACTV_LAUNCH_IFACE ReactVShimLaunch =
{
    ReactVCaptureContext,
    ReactVQuiesce,
    ReactVResumed
};
