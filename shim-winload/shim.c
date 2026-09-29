/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     winload loader interface table and entry point
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

/* FUNCTIONS ******************************************************************/

static
VOID
__cdecl
ReactVLog(
    _In_ ULONG Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Format);
}

REACTV_DEBUG_IFACE ReactVShimDebug =
{
    ReactVLog,
    NULL
};

static REACTV_LOADER_IFACE ReactVLoaderIface =
{
    REACTV_LOADER_IFACE_VERSION,
    sizeof(REACTV_LOADER_IFACE),
    &ReactVShimMemory,
    &ReactVShimSystem,
    &ReactVShimLaunch,
    &ReactVShimDebug
};

NTSTATUS
NTAPI
ReactVWinLoadLaunch(VOID)
{
    return ReactVCoreLaunch(&ReactVLoaderIface);
}
