/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     freeldr system interface: ACPI tables, processors, MSRs, CPUID
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "shim.h"

#ifdef UEFIBOOT
/* EFI system table, source of the RSDP */
#include <uefildr.h>
extern EFI_SYSTEM_TABLE *GlobalSystemTable;
#endif

#include <debug.h>
DBG_DEFAULT_CHANNEL(HYPERVISOR);

/* ACPI table layouts per the ACPI spec. freeldr has no table cache */

#define REACTV_ACPI_MADT_SIGNATURE 0x43495041 /* 'APIC' */

#define REACTV_MADT_LOCAL_APIC   0
#define REACTV_MADT_LOCAL_X2APIC 9

#define REACTV_MADT_ENABLED        0x00000001
#define REACTV_MADT_ONLINE_CAPABLE 0x00000002

#define REACTV_EBDA_SEGMENT_PTR 0x40E
#define REACTV_BIOS_AREA_BASE   0xE0000
#define REACTV_BIOS_AREA_END    0x100000

#include <pshpack1.h>
typedef struct _REACTV_ACPI_RSDP
{
    CHAR Signature[8];
    UCHAR Checksum;
    CHAR OemId[6];
    UCHAR Revision;
    ULONG RsdtAddress;
    ULONG Length;
    ULONG64 XsdtAddress;
    UCHAR ExtendedChecksum;
    UCHAR Reserved[3];
} REACTV_ACPI_RSDP, *PREACTV_ACPI_RSDP;

typedef struct _REACTV_ACPI_HEADER
{
    ULONG Signature;
    ULONG Length;
    UCHAR Revision;
    UCHAR Checksum;
    CHAR OemId[6];
    CHAR OemTableId[8];
    ULONG OemRevision;
    ULONG CreatorId;
    ULONG CreatorRevision;
} REACTV_ACPI_HEADER, *PREACTV_ACPI_HEADER;

typedef struct _REACTV_ACPI_MADT
{
    REACTV_ACPI_HEADER Header;
    ULONG LocalApicAddress;
    ULONG Flags;
} REACTV_ACPI_MADT, *PREACTV_ACPI_MADT;

typedef struct _REACTV_ACPI_MADT_ENTRY
{
    UCHAR Type;
    UCHAR Length;
} REACTV_ACPI_MADT_ENTRY, *PREACTV_ACPI_MADT_ENTRY;

typedef struct _REACTV_ACPI_LOCAL_APIC
{
    UCHAR Type;
    UCHAR Length;
    UCHAR AcpiProcessorId;
    UCHAR ApicId;
    ULONG Flags;
} REACTV_ACPI_LOCAL_APIC, *PREACTV_ACPI_LOCAL_APIC;

typedef struct _REACTV_ACPI_LOCAL_X2APIC
{
    UCHAR Type;
    UCHAR Length;
    USHORT Reserved;
    ULONG ApicId;
    ULONG Flags;
    ULONG AcpiProcessorUid;
} REACTV_ACPI_LOCAL_X2APIC, *PREACTV_ACPI_LOCAL_X2APIC;
#include <poppack.h>

/* FUNCTIONS ******************************************************************/

static
BOOLEAN
NTAPI
ReactVAcpiChecksumIsValid(
    _In_reads_bytes_(Length) PVOID Table,
    _In_ ULONG Length)
{
    PUCHAR Bytes = Table;
    UCHAR Sum = 0;
    ULONG Index;

    for (Index = 0; Index < Length; Index++)
        Sum += Bytes[Index];

    return (Sum == 0);
}

static
PREACTV_ACPI_RSDP
NTAPI
ReactVFindRsdpIn(
    _In_ ULONG_PTR Base,
    _In_ ULONG_PTR End)
{
#ifdef UEFIBOOT
    /* UEFI provides the RSDP directly, nothing to scan */
    UNREFERENCED_PARAMETER(Base);
    UNREFERENCED_PARAMETER(End);
    return NULL;
#else
    PREACTV_ACPI_RSDP Rsdp;
    ULONG_PTR Address;

    for (Address = Base; Address < End; Address += 16)
    {
        Rsdp = (PREACTV_ACPI_RSDP)Address;
        if (memcmp(Rsdp->Signature, "RSD PTR ", 8) != 0)
            continue;

        if (!ReactVAcpiChecksumIsValid(Rsdp, FIELD_OFFSET(REACTV_ACPI_RSDP, Length)))
        {
            RV_WARN("Bad RSDP checksum at %p\n", Rsdp);
            continue;
        }

        return Rsdp;
    }

    return NULL;
#endif
}

static
PREACTV_ACPI_RSDP
NTAPI
ReactVFindRsdp(VOID)
{
#ifdef UEFIBOOT
    EFI_GUID Acpi20 = EFI_ACPI_20_TABLE_GUID;
    UINTN Index;

    /* UEFI has no EBDA or BIOS area to scan; the RSDP is in the configuration table */
    for (Index = 0; Index < GlobalSystemTable->NumberOfTableEntries; Index++)
    {
        if (memcmp(&GlobalSystemTable->ConfigurationTable[Index].VendorGuid,
                   &Acpi20,
                   sizeof(Acpi20)) == 0)
        {
            return (PREACTV_ACPI_RSDP)
                GlobalSystemTable->ConfigurationTable[Index].VendorTable;
        }
    }

    return NULL;
#else
    PREACTV_ACPI_RSDP Rsdp;
    ULONG_PTR EbdaBase;

    /* Scan the first 1KB of the EBDA, then 0xE0000-0xFFFFF */
    EbdaBase = (ULONG_PTR)(*(PUSHORT)(ULONG_PTR)REACTV_EBDA_SEGMENT_PTR) << 4;
    if (EbdaBase != 0)
    {
        Rsdp = ReactVFindRsdpIn(EbdaBase, EbdaBase + 1024);
        if (Rsdp != NULL)
            return Rsdp;
    }

    return ReactVFindRsdpIn(REACTV_BIOS_AREA_BASE, REACTV_BIOS_AREA_END);
#endif
}

/* Map the header first to read the table length */
static
NTSTATUS
NTAPI
ReactVMapAcpiTable(
    _In_ ULONG64 PhysicalAddress,
    _Outptr_ PREACTV_ACPI_HEADER *Table)
{
    PREACTV_ACPI_HEADER Header;
    NTSTATUS Status;

    Status = ReactVShimMapPhysical(PhysicalAddress, sizeof(*Header), (PVOID *)&Header);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Header->Length < sizeof(*Header))
    {
        RV_ERR("Table at %I64x has bad length %lu\n", PhysicalAddress, Header->Length);
        return STATUS_NOT_FOUND;
    }

    return ReactVShimMapPhysical(PhysicalAddress, Header->Length, (PVOID *)Table);
}

static
NTSTATUS
NTAPI
ReactVFindAcpiTable(
    _In_ ULONG Signature,
    _Out_ ULONG64 *PhysicalAddress,
    _Out_opt_ ULONG *Length)
{
    PREACTV_ACPI_RSDP Rsdp;
    PREACTV_ACPI_HEADER Root, Table;
    ULONG64 TableAddress;
    ULONG Count, Index, EntrySize;
    BOOLEAN UseXsdt;
    NTSTATUS Status;

    Rsdp = ReactVFindRsdp();
    if (Rsdp == NULL)
    {
        RV_ERR("RSDP not found\n");
        return STATUS_NOT_FOUND;
    }

    UseXsdt = ((Rsdp->Revision >= 2) && (Rsdp->XsdtAddress != 0));
    TableAddress = UseXsdt ? Rsdp->XsdtAddress : Rsdp->RsdtAddress;
    EntrySize = UseXsdt ? sizeof(ULONG64) : sizeof(ULONG);

    Status = ReactVMapAcpiTable(TableAddress, &Root);
    if (!NT_SUCCESS(Status))
    {
        RV_ERR("Cannot map RSDT/XSDT at %I64x\n", TableAddress);
        return Status;
    }

    Count = (Root->Length - sizeof(*Root)) / EntrySize;
    for (Index = 0; Index < Count; Index++)
    {
        PVOID Entry = (PUCHAR)Root + sizeof(*Root) + (Index * EntrySize);

        if (UseXsdt)
            TableAddress = *(PULONG64)Entry;
        else
            TableAddress = *(PULONG)Entry;

        /* Some firmware leaves null entries */
        if (TableAddress == 0)
            continue;

        if (!NT_SUCCESS(ReactVMapAcpiTable(TableAddress, &Table)))
        {
            RV_WARN("Cannot map table at %I64x\n", TableAddress);
            continue;
        }

        if (Table->Signature != Signature)
            continue;

        if (!ReactVAcpiChecksumIsValid(Table, Table->Length))
            RV_WARN("Bad checksum, table at %I64x\n", TableAddress);

        *PhysicalAddress = TableAddress;
        if (Length != NULL)
            *Length = Table->Length;

        return STATUS_SUCCESS;
    }

    return STATUS_NOT_FOUND;
}

static
NTSTATUS
NTAPI
ReactVQueryProcessors(
    _Inout_ ULONG *ProcessorCount,
    _Out_writes_opt_(*ProcessorCount) PREACTV_PROCESSOR_INFO Processors)
{
    PREACTV_ACPI_MADT Madt;
    PREACTV_ACPI_MADT_ENTRY Entry;
    PUCHAR Current, End;
    ULONG64 MadtAddress;
    ULONG MadtLength, Capacity, Count;
    NTSTATUS Status;

    Status = ReactVFindAcpiTable(REACTV_ACPI_MADT_SIGNATURE, &MadtAddress, &MadtLength);
    if (!NT_SUCCESS(Status))
        return Status;

    if (MadtLength < sizeof(*Madt))
        return STATUS_NOT_FOUND;

    /* Already mapped by the lookup, this only gets the pointer */
    Status = ReactVShimMapPhysical(MadtAddress, MadtLength, (PVOID *)&Madt);
    if (!NT_SUCCESS(Status))
        return Status;

    Capacity = *ProcessorCount;
    Count = 0;

    Current = (PUCHAR)Madt + sizeof(*Madt);
    End = (PUCHAR)Madt + MadtLength;

    while (Current + sizeof(*Entry) <= End)
    {
        Entry = (PREACTV_ACPI_MADT_ENTRY)Current;
        if ((Entry->Length < sizeof(*Entry)) || (Current + Entry->Length > End))
            break;

        if ((Entry->Type == REACTV_MADT_LOCAL_APIC) &&
            (Entry->Length >= sizeof(REACTV_ACPI_LOCAL_APIC)))
        {
            PREACTV_ACPI_LOCAL_APIC LocalApic = (PREACTV_ACPI_LOCAL_APIC)Current;

            if ((Processors != NULL) && (Count < Capacity))
            {
                Processors[Count].ApicId = LocalApic->ApicId;
                Processors[Count].NumaNode = 0;
                Processors[Count].Flags = LocalApic->Flags &
                    (REACTV_MADT_ENABLED | REACTV_MADT_ONLINE_CAPABLE);
                Processors[Count].Reserved = 0;
            }
            Count++;
        }
        else if ((Entry->Type == REACTV_MADT_LOCAL_X2APIC) &&
                 (Entry->Length >= sizeof(REACTV_ACPI_LOCAL_X2APIC)))
        {
            PREACTV_ACPI_LOCAL_X2APIC LocalApic = (PREACTV_ACPI_LOCAL_X2APIC)Current;

            if ((Processors != NULL) && (Count < Capacity))
            {
                Processors[Count].ApicId = LocalApic->ApicId;
                Processors[Count].NumaNode = 0;
                Processors[Count].Flags = LocalApic->Flags &
                    (REACTV_MADT_ENABLED | REACTV_MADT_ONLINE_CAPABLE);
                Processors[Count].Reserved = 0;
            }
            Count++;
        }

        Current += Entry->Length;
    }

    *ProcessorCount = Count;

    if ((Processors == NULL) || (Capacity < Count))
        return STATUS_BUFFER_TOO_SMALL;

    return STATUS_SUCCESS;
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
            RV_ERR("Invalid CR%lu\n", Number);
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
