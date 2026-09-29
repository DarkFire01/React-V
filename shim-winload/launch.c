/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     winload launch interface (stubs)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
ReactVCaptureContext(
    _Out_ PREACTV_LAUNCH_CONTEXT Context)
{
    RtlZeroMemory(Context, sizeof(*Context));
    Context->Size = sizeof(*Context);
}

static
VOID
NTAPI
ReactVQuiesce(VOID)
{
    /* Boot debugger must be off during launch */
}

static
VOID
NTAPI
ReactVResumed(
    _In_ NTSTATUS Status)
{
    /* Restart the boot debugger and record a successful launch */
    UNREFERENCED_PARAMETER(Status);
}

REACTV_LAUNCH_IFACE ReactVShimLaunch =
{
    ReactVCaptureContext,
    ReactVQuiesce,
    ReactVResumed
};
