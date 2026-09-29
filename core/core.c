/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Core entry point and loader interface validation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"

/* GLOBALS ********************************************************************/

REACTV_CORE ReactVCore;

/* FUNCTIONS ******************************************************************/

static
BOOLEAN
NTAPI
ReactVValidateInterface(
    _In_ PREACTV_LOADER_IFACE Loader)
{
    if (Loader->Version != REACTV_LOADER_IFACE_VERSION)
    {
        ReactVLogError("Loader interface version %lu, expected %lu\n",
                       Loader->Version,
                       (ULONG)REACTV_LOADER_IFACE_VERSION);
        return FALSE;
    }

    if (Loader->Size < sizeof(*Loader))
    {
        ReactVLogError("Loader interface size %lu, expected at least %lu\n",
                       Loader->Size,
                       (ULONG)sizeof(*Loader));
        return FALSE;
    }

    if ((Loader->Memory == NULL) || (Loader->System == NULL) ||
        (Loader->Launch == NULL) || (Loader->Debug == NULL))
    {
        ReactVLogError("Loader interface missing memory, system, launch or debug table\n");
        return FALSE;
    }

    if (Loader->Debug->Log == NULL)
        return FALSE;

    if ((Loader->Memory->ReserveCoreImage == NULL) ||
        (Loader->Memory->AllocatePages == NULL) ||
        (Loader->Memory->FreePages == NULL) ||
        (Loader->Memory->AllocateStartupPage == NULL) ||
        (Loader->Memory->MapPhysical == NULL) ||
        (Loader->Memory->UnmapVirtual == NULL) ||
        (Loader->Memory->TranslateVirtual == NULL) ||
        (Loader->Memory->GetMemoryMap == NULL) ||
        (Loader->Memory->GetHighestPhysicalPage == NULL))
    {
        ReactVLogError("Loader memory table incomplete\n");
        return FALSE;
    }

    if ((Loader->System->FindAcpiTable == NULL) ||
        (Loader->System->QueryProcessors == NULL) ||
        (Loader->System->ReadMsr == NULL) ||
        (Loader->System->ReadControlRegister == NULL) ||
        (Loader->System->Cpuid == NULL))
    {
        ReactVLogError("Loader system table incomplete\n");
        return FALSE;
    }

    if ((Loader->Launch->CaptureContext == NULL) ||
        (Loader->Launch->Quiesce == NULL) ||
        (Loader->Launch->Resumed == NULL))
    {
        ReactVLogError("Loader launch table incomplete\n");
        return FALSE;
    }

    return TRUE;
}

/*
 * Build a per-processor record from the loader's processor list. Only the BSP
 * is virtualized here; the AP records are for starting them later.
 */
static
VOID
NTAPI
ReactVCountProcessors(VOID)
{
    REACTV_PROCESSOR_INFO Found[REACTV_MAXIMUM_PROCESSORS];
    ULONG Count = RTL_NUMBER_OF(Found);
    ULONG Index;
    NTSTATUS Status;

    RtlZeroMemory(Found, sizeof(Found));

    /* Fall back to a single processor if the query fails */
    Status = ReactVCore.Loader->System->QueryProcessors(&Count, Found);
    if (!NT_SUCCESS(Status) || (Count == 0) || (Count > RTL_NUMBER_OF(Found)))
    {
        ReactVLogWarning("Processor query failed, status %08lx, assuming 1\n",
                         Status);
        Count = 1;
    }

    ReactVCore.ProcessorCount = Count;

    for (Index = 0; Index < Count; Index++)
    {
        ReactVCore.Processor[Index].Self = &ReactVCore.Processor[Index];
        ReactVCore.Processor[Index].Index = Index;
        ReactVCore.Processor[Index].ApicId = Found[Index].ApicId;
    }

    ReactVLogTrace("%lu processor(s), virtualizing the BSP\n",
                   Count);
}

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVCoreLaunch(
    _In_ PREACTV_LOADER_IFACE Loader)
{
    NTSTATUS Status;

    /* Before anything that can fail, so launch failures are logged */
    ReactVSerialInitialize();

    ReactVLogTrace("ReactV launch, loader interface version %lu\n",
                   Loader->Version);

    if (!ReactVValidateInterface(Loader))
    {
        ReactVLogError("Loader interface rejected\n");
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&ReactVCore, sizeof(ReactVCore));
    ReactVCore.Loader = Loader;

    Status = ReactVDetectBackend();
    if (!NT_SUCCESS(Status))
        return Status;

    ReactVCountProcessors();

    /* All core memory must be reserved and mapped before launch */
    Status = ReactVCreateAddressSpace();
    if (!NT_SUCCESS(Status))
        return Status;

    /*
     * SynIC setup runs before launch. On failure the partition privileges are
     * not advertised, so the root will not create partitions.
     */
    ReactVCore.SynicReady = ReactVSynicInitialize();

    /* Root partition, parent of all child partitions */
    ReactVPartitionInitialize();

    ReactVCore.Loader->Launch->CaptureContext(&ReactVCore.Root);
    ReactVReportRootContext(&ReactVCore.Root);

    /* Returns in the root as a guest on success, or unvirtualized on failure */
    if (ReactVCore.Backend == ReactVBackendVmx)
        Status = ReactVVmxLaunchRoot();
    else
        Status = ReactVSvmLaunchRoot();

    ReactVCore.Loader->Launch->Resumed(Status);

    if (NT_SUCCESS(Status))
        ReactVLogTrace("Root running as guest\n");

    return Status;
}
