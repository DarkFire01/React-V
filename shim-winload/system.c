/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     winload system interface: ACPI tables, processors, MSRs, CPUID
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

/* FUNCTIONS ******************************************************************/

static
NTSTATUS
NTAPI
ReactVFindAcpiTable(
    _In_ ULONG Signature,
    _Out_ ULONG64 *PhysicalAddress,
    _Out_opt_ ULONG *Length)
{
    /* Look up the table through the boot library ACPI cache */
    UNREFERENCED_PARAMETER(Signature);
    UNREFERENCED_PARAMETER(PhysicalAddress);
    UNREFERENCED_PARAMETER(Length);

    return STATUS_NOT_IMPLEMENTED;
}

static
NTSTATUS
NTAPI
ReactVQueryProcessors(
    _Inout_ ULONG *ProcessorCount,
    _Out_writes_opt_(*ProcessorCount) PREACTV_PROCESSOR_INFO Processors)
{
    /* Processor count and APIC IDs from the boot library */
    UNREFERENCED_PARAMETER(Processors);

    *ProcessorCount = 0;
    return STATUS_NOT_IMPLEMENTED;
}

static
ULONG64
NTAPI
ReactVReadMsr(
    _In_ ULONG Index)
{
    return __readmsr(Index);
}

static
ULONG64
NTAPI
ReactVReadControlRegister(
    _In_ ULONG Number)
{
    switch (Number)
    {
        case 0:
            return __readcr0();
        case 2:
            return __readcr2();
        case 3:
            return __readcr3();
        case 4:
            return __readcr4();
        case 8:
            return __readcr8();
        default:
            return 0;
    }
}

static
VOID
NTAPI
ReactVCpuid(
    _In_ ULONG Leaf,
    _In_ ULONG SubLeaf,
    _Out_writes_(4) ULONG *Registers)
{
    int Result[4];

    __cpuidex(Result, (int)Leaf, (int)SubLeaf);

    Registers[0] = (ULONG)Result[0];
    Registers[1] = (ULONG)Result[1];
    Registers[2] = (ULONG)Result[2];
    Registers[3] = (ULONG)Result[3];
}

REACTV_SYSTEM_IFACE ReactVShimSystem =
{
    ReactVFindAcpiTable,
    ReactVQueryProcessors,
    ReactVReadMsr,
    ReactVReadControlRegister,
    ReactVCpuid
};
