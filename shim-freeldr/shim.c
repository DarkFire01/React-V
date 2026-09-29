/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     freeldr loader interface table and entry point
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

#include <ntldr/ntldropts.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(HYPERVISOR);

#define REACTV_LOG_BUFFER_SIZE 256

#define REACTV_LAUNCH_OPTION "HYPERVISORLAUNCHTYPE="

/* FUNCTIONS ******************************************************************/

VOID
__cdecl
ReactVShimLog(
    _In_ ULONG Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
#if DBG
    CHAR Buffer[REACTV_LOG_BUFFER_SIZE];
    va_list Arguments;

    va_start(Arguments, Format);
    _vsnprintf(Buffer, sizeof(Buffer), Format, Arguments);
    va_end(Arguments);
    Buffer[sizeof(Buffer) - 1] = ANSI_NULL;

    /* Use the core serial port once it is up so shim and core output stay ordered */
    if (ReactVSerialReady())
    {
        ReactVSerialPut("reactv: ");
        ReactVSerialPut(Buffer);
        return;
    }

    switch (Level)
    {
        case REACTV_LOG_ERROR:
            ERR("%s", Buffer);
            break;

        case REACTV_LOG_WARNING:
            WARN("%s", Buffer);
            break;

        default:
            TRACE("%s", Buffer);
            break;
    }
#else
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Format);
#endif
}

REACTV_DEBUG_IFACE ReactVShimDebug =
{
    ReactVShimLog,
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

static
BOOLEAN
NTAPI
ReactVLaunchRequested(
    _In_ PCSTR BootOptions)
{
    PCSTR Option;
    ULONG Length = 0;

    Option = NtLdrGetOptionEx(BootOptions, REACTV_LAUNCH_OPTION, &Length);
    if (Option == NULL)
    {
        /* Log it, otherwise this looks the same as a failed launch */
        RV_TRACE("No " REACTV_LAUNCH_OPTION " option, not launching\n");
        return FALSE;
    }

    /* Skip the option name */
    if (Length <= sizeof(REACTV_LAUNCH_OPTION) - 1)
    {
        RV_WARN("Launch type has no value\n");
        return FALSE;
    }

    Option += sizeof(REACTV_LAUNCH_OPTION) - 1;
    Length -= sizeof(REACTV_LAUNCH_OPTION) - 1;

    if ((Length == 4) && (_strnicmp(Option, "AUTO", 4) == 0))
    {
        RV_TRACE("Launch type AUTO\n");
        return TRUE;
    }

    if ((Length == 3) && (_strnicmp(Option, "OFF", 3) == 0))
        return FALSE;

    RV_WARN("Unknown launch type '%.*s'\n", Length, Option);
    return FALSE;
}

VOID
NTAPI
ReactVFreeLdrLaunch(
    _In_ PCSTR BootOptions)
{
    NTSTATUS Status;

    /* Init serial first so option parsing logs go to the core port. The core's later init is a no-op */
    ReactVSerialInitialize();

    if (!ReactVLaunchRequested(BootOptions))
        return;

    Status = ReactVCoreLaunch(&ReactVLoaderIface);
    if (!NT_SUCCESS(Status))
    {
        RV_ERR("Core launch failed %lx\n", Status);
        return;
    }

    RV_TRACE("Core launched, loader is the root partition\n");

    /* Probe the hypervisor interface from the root */
    ReactVProbe();
}
