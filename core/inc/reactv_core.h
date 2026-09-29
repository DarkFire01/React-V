/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Core internal state and helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_CORE_H_
#define _REACTV_CORE_H_

#include "reactv_loader.h"
#include "reactv_paging.h"
#include "reactv_svm.h"
#include "reactv_vmcs.h"
#include "reactv_tlfs.h"

/* GPRs saved on #VMEXIT, in push order. RAX and RSP live in the VMCB. */
typedef struct _REACTV_GUEST_REGISTERS
{
    ULONG64 R15;
    ULONG64 R14;
    ULONG64 R13;
    ULONG64 R12;
    ULONG64 R11;
    ULONG64 R10;
    ULONG64 R9;
    ULONG64 R8;
    ULONG64 Rdi;
    ULONG64 Rsi;
    ULONG64 Rbp;
    ULONG64 Rbx;
    ULONG64 Rdx;
    ULONG64 Rcx;
} REACTV_GUEST_REGISTERS, *PREACTV_GUEST_REGISTERS;

/*
 * Host stack frame built by the SVM exit path. XMM0-5 carry extended fast
 * hypercall input and are saved before the core clobbers them.
 */
typedef struct _REACTV_GUEST_STATE
{
    ULONG64 Xmm[6][2];
    REACTV_GUEST_REGISTERS Registers;
} REACTV_GUEST_STATE, *PREACTV_GUEST_STATE;

C_ASSERT(FIELD_OFFSET(REACTV_GUEST_STATE, Registers) == 96);

/*
 * VMX exit frame. VMCS does not hold RAX, so the stub saves it first (last
 * field). The rest matches REACTV_GUEST_STATE so shared code handles both.
 */
typedef struct _REACTV_VMX_GUEST_STATE
{
    ULONG64 Xmm[6][2];
    REACTV_GUEST_REGISTERS Registers;
    ULONG64 Rax;
} REACTV_VMX_GUEST_STATE, *PREACTV_VMX_GUEST_STATE;

C_ASSERT(FIELD_OFFSET(REACTV_VMX_GUEST_STATE, Registers) ==
         FIELD_OFFSET(REACTV_GUEST_STATE, Registers));

typedef enum _REACTV_BACKEND
{
    ReactVBackendNone = 0,
    ReactVBackendSvm,
    ReactVBackendVmx
} REACTV_BACKEND;

/* Core page tables: allocations identity mapped, image at its link address */
typedef struct _REACTV_ADDRESS_SPACE
{
    ULONG64 Pml4;
    ULONG64 TablePoolBase;
    ULONG64 TablePoolPages;
    ULONG64 TablePoolUsed;
} REACTV_ADDRESS_SPACE, *PREACTV_ADDRESS_SPACE;

/* Static limits, records live in the core image */
#define REACTV_MAXIMUM_PARTITIONS 4
#define REACTV_MAXIMUM_VPS        4

/* Host processors supported, one 64-bit affinity mask; extras are left alone */
#define REACTV_MAXIMUM_PROCESSORS 64

/* Ports are owned by one partition; a connection is another partition's handle to a port */
#define REACTV_MAXIMUM_PORTS       16
#define REACTV_MAXIMUM_CONNECTIONS 16
#define REACTV_PORT_PROPERTIES     8

typedef struct _REACTV_PORT
{
    BOOLEAN Used;
    ULONG Id;
    ULONG Type;
    ULONG64 OwnerPartitionId;
    /* Partition allowed to connect */
    ULONG64 ConnectionPartitionId;
    ULONG TargetVp;
    ULONG TargetSint;
    USHORT BaseFlagNumber;
    USHORT FlagCount;
    ULONG64 MonitorAddress;
    /* Delivery count, read back by the root as a port property */
    ULONG64 Taken;
    ULONG64 Property[REACTV_PORT_PROPERTIES];
} REACTV_PORT, *PREACTV_PORT;

typedef struct _REACTV_CONNECTION
{
    BOOLEAN Used;
    ULONG Id;
    ULONG64 PartitionId;
    ULONG PortIndex;
} REACTV_CONNECTION, *PREACTV_CONNECTION;

/* Root partition ID and ASID */
#define REACTV_ROOT_PARTITION_ID 1
#define REACTV_ROOT_ASID         1

/* Child VP x2APIC state, only the registers guests use */
#define REACTV_APIC_VECTOR_WORDS 8

typedef struct _REACTV_VP_APIC
{
    ULONG64 Base;
    ULONG Id;
    ULONG TaskPriority;
    ULONG Spurious;
    ULONG LogicalId;
    ULONG ErrorStatus;
    ULONG64 Command;
    /* VP assist page; auto EOI is never offered */
    ULONG64 AssistPage;
    /* IRR and ISR bitmaps */
    ULONG Requested[REACTV_APIC_VECTOR_WORDS];
    ULONG InService[REACTV_APIC_VECTOR_WORDS];
    /* LVT and timer registers */
    ULONG Timer;
    ULONG TimerInitial;
    ULONG TimerDivide;
    ULONG Error;
    /* AP waiting for SIPI; never set on the BSP */
    BOOLEAN WaitingForStartup;
} REACTV_VP_APIC, *PREACTV_VP_APIC;

/* Per VP SynIC state for child partitions. The root's SynIC is kept globally. */
typedef struct _REACTV_VP_SYNIC
{
    ULONG64 Control;
    /* SIMP and SIEFP */
    ULONG64 MessagePage;
    ULONG64 EventPage;
    ULONG64 Sint[HV_SYNIC_SINT_COUNT];
    /* Synthetic timer config, count and next expiration */
    ULONG64 TimerConfig[HV_SYNIC_STIMER_COUNT];
    ULONG64 TimerCount[HV_SYNIC_STIMER_COUNT];
    ULONG64 TimerDue[HV_SYNIC_STIMER_COUNT];
    /* SINTs with undelivered interrupts, one bit each */
    ULONG Pending;
    /* SINT last delivered, whose message slot EOM clears */
    ULONG Serviced;
} REACTV_VP_SYNIC, *PREACTV_VP_SYNIC;

typedef struct _REACTV_VP
{
    BOOLEAN Created;
    BOOLEAN Started;
    ULONG Index;
    ULONG64 Vmcb;
    /* x87/SSE save area, not part of the VMCB */
    ULONG64 FpuState;
    /* GPRs not held in the VMCB (RAX, RSP, RIP etc. are) */
    REACTV_GUEST_REGISTERS Registers;
    /* Deliverability notification register and NMI mask */
    ULONG64 Deliverability;
    BOOLEAN NmiMasked;
    /* Blocked dispatch already logged */
    BOOLEAN ToldBlocked;
    /* Suspend registers set by the root */
    ULONG64 ExplicitSuspend;
    ULONG64 InterceptSuspend;
    ULONG64 DispatchSuspend;
    ULONG64 EmulationHints;
    /* VP stats page */
    ULONG64 StatsPage;
    /* VP state pages shared with the root */
    ULONG64 StatePage[HV_VP_STATE_PAGE_TYPES];
    REACTV_VP_SYNIC Synic;
    REACTV_VP_APIC Apic;
} REACTV_VP, *PREACTV_VP;

/* Properties stored for read back but not acted on; each write is logged */
#define REACTV_RECORDED_PROPERTIES 12

typedef struct _REACTV_PROPERTY
{
    ULONG Code;
    ULONG64 Value;
} REACTV_PROPERTY, *PREACTV_PROPERTY;

/* Per leaf CPUID intercepts; overflow falls back to intercepting every leaf */
#define REACTV_CPUID_INTERCEPTS 16

typedef struct _REACTV_INTERCEPTS
{
    BOOLEAN EveryCpuid;
    BOOLEAN EveryMsr;
    BOOLEAN EveryHypercall;
    ULONG CpuidCount;
    ULONG CpuidLeaf[REACTV_CPUID_INTERCEPTS];
    ULONG64 ExceptionVectors;
} REACTV_INTERCEPTS, *PREACTV_INTERCEPTS;

typedef struct _REACTV_PARTITION
{
    BOOLEAN Used;
    BOOLEAN Initialized;
    BOOLEAN Finalized;
    ULONG64 Id;
    ULONG64 CreationFlags;
    ULONG Asid;
    /* NPT root, empty until GPA pages are mapped */
    ULONG64 Npt;
    ULONG64 PrivilegeFlags;
    ULONG64 GuestOsId;
    /* Guest HV_X64_MSR_HYPERCALL value */
    ULONG64 HypercallMsr;
    BOOLEAN HypercallStubWanted;
    ULONG GpaSpaceOptions;
    /* Partition stats page */
    ULONG64 StatsPage;
    /* Doorbell page used for user mode dispatch */
    ULONG64 DoorbellPage;
    ULONG64 ProcessorCount;
    ULONG64 CpuReserve;
    ULONG64 CpuCap;
    ULONG64 CpuWeight;
    /* Deposited pages, linked through the pages themselves */
    ULONG64 DepositedList;
    ULONG64 DepositedPages;
    ULONG64 DepositedUsed;
    ULONG64 MappedPages;
    /* Optional intercepts requested by the root */
    REACTV_INTERCEPTS Intercepts;
    REACTV_PROPERTY Recorded[REACTV_RECORDED_PROPERTIES];
    REACTV_VP Vp[REACTV_MAXIMUM_VPS];
} REACTV_PARTITION, *PREACTV_PARTITION;

/*
 * Per host processor state, only touched by its owner so no locking. IOPM and
 * MSRPM are read only once live and shared from REACTV_CORE.
 * Self must be at offset 0: host GS base points here and survives #VMEXIT.
 */
typedef struct _REACTV_PROCESSOR
{
    struct _REACTV_PROCESSOR *Self;
    ULONG Index;
    ULONG ApicId;
    /* VM_HSAVE_PA area */
    ULONG64 HostSaveArea;
    /* Host stack used on #VMEXIT */
    ULONG64 HostStackTop;
    /* Root VMCB for this processor */
    ULONG64 Vmcb;
    /* Host segment state saved while a child runs */
    ULONG64 HostVmcb;
    /* Host FPU state saved while a child runs */
    ULONG64 HostFpuState;
    /* VMX only */
    ULONG64 VmxOnRegion;
    ULONG64 Vmcs;
    /* SIPI vector (start page number) the root sent */
    ULONG StartupVector;
    /* Running the root under the core */
    BOOLEAN Live;
} REACTV_PROCESSOR, *PREACTV_PROCESSOR;

C_ASSERT(FIELD_OFFSET(REACTV_PROCESSOR, Self) == 0);

typedef struct _REACTV_CORE
{
    PREACTV_LOADER_IFACE Loader;
    REACTV_BACKEND Backend;
    ULONG SvmRevision;
    ULONG SvmAsidCount;
    ULONG SvmFeatures;
    ULONG64 ImageBase;
    ULONG64 ImageLength;
    REACTV_ADDRESS_SPACE Space;
    /* Processor count from the loader */
    ULONG ProcessorCount;
    REACTV_PROCESSOR Processor[REACTV_MAXIMUM_PROCESSORS];
    ULONG64 Iopm;
    ULONG64 Msrpm;
    ULONG64 VmxMsrBitmap;
    /* All ones: intercept every child I/O port and MSR access */
    ULONG64 ChildIopm;
    ULONG64 ChildMsrpm;
    /* Set once the loader mapping is no longer usable */
    BOOLEAN Live;
    /* Root TLFS guest interface MSRs */
    ULONG64 GuestOsId;
    ULONG64 HypercallMsr;
    /* SynIC MSRs can be exposed */
    BOOLEAN SynicReady;
    /* End of the physical memory window (exclusive) */
    ULONG64 MemoryLimit;
    ULONG64 NextPartitionId;
    /* Real mode AP startup trampoline page, below 1MB */
    ULONG64 StartupPage;
    /* System properties set by the root, stored only */
    REACTV_PROPERTY System[REACTV_RECORDED_PROPERTIES];

    REACTV_PARTITION Partition[REACTV_MAXIMUM_PARTITIONS];
    REACTV_LAUNCH_CONTEXT Root;
} REACTV_CORE, *PREACTV_CORE;

extern REACTV_CORE ReactVCore;

/*
 * Current processor record via the host GS base, which #VMEXIT restores.
 * Only valid after GS base is set, right before the first guest entry;
 * earlier code indexes ReactVCore.Processor directly.
 */
FORCEINLINE
PREACTV_PROCESSOR
ReactVCurrentProcessor(VOID)
{
    return (PREACTV_PROCESSOR)__readgsqword(0);
}

/* ap.c */

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVApInitialize(VOID);

/* Starts a host AP under the core, running the root from GuestVector. FALSE on any failure. */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVApStart(
    _In_ ULONG ApicId,
    _In_ ULONG GuestVector);

/* TRUE if the processor with this APIC ID is already live */
BOOLEAN
NTAPI
ReactVApIsLive(
    _In_ ULONG ApicId);

VOID
NTAPI
ReactVApEnter(
    _In_ ULONG64 Index);

/* Enters the core on the current processor. Does not return on success. */
_Must_inspect_result_
NTSTATUS
NTAPI
ReactVSvmEnterAsProcessor(
    _In_ PREACTV_PROCESSOR Processor);

/* serial.c */
VOID
__cdecl
ReactVLog(
    _In_ ULONG Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

VOID
NTAPI
ReactVSerialInitialize(VOID);

#define ReactVLogError(...)   ReactVLog(REACTV_LOG_ERROR, __VA_ARGS__)
#define ReactVLogWarning(...) ReactVLog(REACTV_LOG_WARNING, __VA_ARGS__)
#define ReactVLogTrace(...)   ReactVLog(REACTV_LOG_TRACE, __VA_ARGS__)

/* detect.c */

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVDetectBackend(VOID);

VOID
NTAPI
ReactVReportRootContext(
    _In_ PREACTV_LAUNCH_CONTEXT Context);

/* paging.c */

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVCreateAddressSpace(VOID);

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVMapCoreRange(
    _In_ ULONG64 VirtualBase,
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length);

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVMapCoreDevice(
    _In_ ULONG64 VirtualBase,
    _In_ ULONG64 PhysicalBase,
    _In_ ULONG64 Length);

/* synic.c, root SynIC and synthetic MSRs */

BOOLEAN
NTAPI
ReactVSynicInitialize(VOID);

ULONG64
NTAPI
ReactVReferenceTime(VOID);

ULONG64
NTAPI
ReactVCyclesPerSecond(VOID);

VOID
NTAPI
ReactVWriteHypercallStub(
    _In_ ULONG64 PageBase);

VOID
NTAPI
ReactVWriteDoorbellStub(
    _In_ ULONG64 PageBase,
    _In_ ULONG64 PartitionId);

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicReadMsr(
    _In_ ULONG Index,
    _Out_ ULONG64 *Value);

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicWriteMsr(
    _In_ ULONG Index,
    _In_ ULONG64 Value);

/* Allocates zeroed, identity mapped core pages */
_Must_inspect_result_
NTSTATUS
NTAPI
ReactVAllocateMapped(
    _In_ ULONG64 PageCount,
    _In_ ULONG64 AlignmentInPages,
    _In_ ULONG Tag,
    _Out_ ULONG64 *PhysicalBase);

/* svm.c */

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVSvmLaunchRoot(VOID);

/* arch/amd64/svm.S */

/* lgdt/lidt operand */
#include <pshpack1.h>
typedef struct _REACTV_DESCRIPTOR_REGISTER
{
    USHORT Limit;
    ULONG64 Base;
} REACTV_DESCRIPTOR_REGISTER, *PREACTV_DESCRIPTOR_REGISTER;
#include <poppack.h>

C_ASSERT(sizeof(REACTV_DESCRIPTOR_REGISTER) == 10);

/* Loads the root's GDT/IDT and selectors on this processor */
VOID
NTAPI
ReactVSvmAdoptDescriptors(
    _In_ PVOID Gdtr,
    _In_ PVOID Idtr,
    _In_ ULONG64 Code,
    _In_ ULONG64 Data);

/* ReactVSvmEnterGuest for an AP starting from reset */
BOOLEAN
NTAPI
ReactVSvmEnterGuestAp(
    _In_ ULONG64 Vmcb,
    _In_ ULONG64 HostCr3,
    _In_ ULONG64 HostStackTop);

/* TRUE once the root resumes as a guest, FALSE if vmrun failed */
BOOLEAN
NTAPI
ReactVSvmEnterGuest(
    _In_ ULONG64 Vmcb,
    _In_ ULONG64 HostCr3,
    _In_ ULONG64 HostStackTop);

/* Guest RIP target in the VMCB, never called */
VOID
NTAPI
ReactVSvmGuestResume(VOID);

/* vmx.c */

_Must_inspect_result_
NTSTATUS
NTAPI
ReactVVmxLaunchRoot(VOID);

/* arch/amd64/vmx.S */

BOOLEAN
NTAPI
ReactVVmxOn(
    _In_ ULONG64 Region);

VOID
NTAPI
ReactVVmxOff(VOID);

BOOLEAN
NTAPI
ReactVVmxClear(
    _In_ ULONG64 Vmcs);

BOOLEAN
NTAPI
ReactVVmxPtrLoad(
    _In_ ULONG64 Vmcs);

BOOLEAN
NTAPI
ReactVVmxWrite(
    _In_ ULONG Field,
    _In_ ULONG64 Value);

ULONG64
NTAPI
ReactVVmxRead(
    _In_ ULONG Field);

/* TRUE once the root resumes as a guest, FALSE if vmlaunch failed */
BOOLEAN
NTAPI
ReactVVmxEnterGuest(
    _In_ ULONG64 Vmcs,
    _In_ ULONG64 HostCr3,
    _In_ ULONG64 HostStackTop);

/* VMCS guest RIP, never called */
VOID
NTAPI
ReactVVmxGuestResume(VOID);

/* VMCS host RIP, never called */
VOID
NTAPI
ReactVVmxExitEntry(VOID);

/* vmxexit.c */

/* FALSE if the guest never ran; the core does not re-enter */
BOOLEAN
NTAPI
ReactVVmxHandleExit(
    _In_ PREACTV_VMX_GUEST_STATE State);

/* exit.c */

/* FALSE if the guest never ran; unwinds the launch */
BOOLEAN
NTAPI
ReactVSvmHandleExit(
    _In_ ULONG64 VmcbPhysical,
    _In_ PREACTV_GUEST_STATE State);

/* hv.c */

VOID
NTAPI
ReactVHvCpuid(
    _In_ ULONG Leaf,
    _In_ ULONG SubLeaf,
    _Inout_updates_(4) ULONG *Registers);

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVHvReadMsr(
    _In_ ULONG Index,
    _Out_ ULONG64 *Value);

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVHvWriteMsr(
    _In_ ULONG Index,
    _In_ ULONG64 Value);

ULONG64
NTAPI
ReactVHvHypercall(
    _In_ ULONG64 Control,
    _In_ ULONG64 InputGpa,
    _In_ ULONG64 OutputGpa,
    _In_ ULONG64 CallerCr3,
    _In_ PREACTV_GUEST_STATE State);

/* Hypercalls issued by a child partition guest */
ULONG64
NTAPI
ReactVHvGuestHypercall(
    _In_ struct _REACTV_PARTITION *Partition,
    _In_ ULONG64 Control,
    _In_ ULONG64 InputGpa,
    _In_ ULONG64 OutputGpa);

/*
 * Decoded hypercall. Memory calls pass input/output GPAs; fast calls pass 16
 * bytes in RDX:R8, extended fast calls 16 more in each of XMM0-5.
 */
typedef struct _REACTV_HYPERCALL
{
    ULONG64 Control;
    ULONG64 Code;
    ULONG64 RepCount;
    BOOLEAN Fast;
    /* Input/output are caller VAs, translated through CallerCr3 */
    BOOLEAN Virtual;
    ULONG64 CallerCr3;
    /* Calling child partition, NULL for the root */
    struct _REACTV_PARTITION *Caller;
    /* Memory call GPAs, or RDX and R8 for a fast call */
    ULONG64 InputGpa;
    ULONG64 OutputGpa;
    /* Fast call payload: RDX, R8, then XMM0-5 */
    ULONG64 Payload[2 + (6 * 2)];
    PREACTV_GUEST_STATE State;
} REACTV_HYPERCALL, *PREACTV_HYPERCALL;

/* partition.c */

VOID
NTAPI
ReactVPartitionInitialize(VOID);

/* Partition management hypercalls. FALSE if the call code is not handled here. */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVPartitionHypercall(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *Status,
    _Out_ ULONG64 *RepsCompleted);

/* GPA access, GVA translation and VP state hypercalls. FALSE if not handled here. */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVGuestHypercall(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *Status,
    _Out_ ULONG64 *RepsCompleted);

/* ReactVGuestPage results: GPA backing and guest access */
#define REACTV_GUEST_PAGE_ABSENT    0
#define REACTV_GUEST_PAGE_WITHHELD  1
#define REACTV_GUEST_PAGE_READ_ONLY 2
#define REACTV_GUEST_PAGE_PRESENT   3

/* Partition lookup by ID, by caller, and the root */
PREACTV_PARTITION
NTAPI
ReactVPartitionFromId(
    _In_ ULONG64 Id);

PREACTV_PARTITION
NTAPI
ReactVCallerPartition(
    _In_ PREACTV_HYPERCALL Call);

PREACTV_PARTITION
NTAPI
ReactVPartitionFromCall(
    _In_ PREACTV_HYPERCALL Call,
    _In_ ULONG64 Id);

PREACTV_PARTITION
NTAPI
ReactVRootPartition(VOID);

/* Host pointer to a guest physical range within one page */
_Must_inspect_result_
PVOID
NTAPI
ReactVGuestPointer(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _In_ ULONG64 Length);

/* Guest physical memory access, may not cross a page */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVGuestRead(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _Out_writes_bytes_all_(Length) PVOID Buffer,
    _In_ ULONG Length);

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVGuestWrite(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length);

/* synic.c, per VP child SynIC */

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicChildMsr(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Index,
    _In_ BOOLEAN Write,
    _Inout_ PULONG64 Value);

/* apic.c */

/* Sets the VP's APIC to its reset state */
VOID
NTAPI
ReactVApicReset(
    _In_ PREACTV_VP Vp);

/* APIC MSR access. FALSE if Index is not an APIC MSR. */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVApicMsr(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Index,
    _In_ BOOLEAN Write,
    _Inout_ PULONG64 Value);

/* Next APIC vector to inject, or zero */
ULONG
NTAPI
ReactVApicTakePending(
    _In_ PREACTV_VP Vp);

/* TRUE if any APIC interrupt is pending */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVApicWaiting(
    _In_ PREACTV_VP Vp);

/* Writes the hypercall page at the GPA set in the guest's hypercall MSR */
VOID
NTAPI
ReactVSynicWriteCallPage(
    _In_ PREACTV_PARTITION Partition);

/* Fires the VP's expired synthetic timers */
VOID
NTAPI
ReactVSynicTimers(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp);

/* Post a message or signal an event to the root SynIC */
ULONG64
NTAPI
ReactVSynicPostToRoot(
    _In_ ULONG Sint,
    _In_ ULONG PortId,
    _In_ PHV_MESSAGE Message);

ULONG64
NTAPI
ReactVSynicSignalRoot(
    _In_ ULONG Sint,
    _In_ ULONG PortId,
    _In_ ULONG Flag,
    _Out_ PBOOLEAN Newly);

/* TRUE if the VP is suspended or waiting for SIPI and cannot be dispatched */
_Must_inspect_result_
BOOLEAN
NTAPI
ReactVVpHeld(
    _In_ PREACTV_VP Vp);

/* Notifies the root that a parked VP is runnable again */
VOID
NTAPI
ReactVSynicSignalRunnable(
    _In_ ULONG64 PartitionId,
    _In_ ULONG VpIndex);

/* Injects pending root SINTs; only possible on entry to the root */
VOID
NTAPI
ReactVSynicDeliverToRoot(
    _In_ struct _REACTV_VMCB *Vmcb);

/* Post a message or signal an event to a child VP's SynIC */
ULONG64
NTAPI
ReactVSynicPostMessage(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Sint,
    _In_ PHV_MESSAGE Message);

ULONG64
NTAPI
ReactVSynicSignalEvent(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _In_ ULONG Sint,
    _In_ ULONG Flag,
    _Out_ PBOOLEAN Newly);

/* ports.c */

/* Allocates a core page that is never freed */
NTSTATUS
NTAPI
ReactVTakeCorePage(
    _Out_ ULONG64 *PhysicalBase);

/* Frees ports and connections of a deleted partition */
VOID
NTAPI
ReactVReleasePorts(
    _In_ ULONG64 PartitionId);

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVPortHypercall(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *Status,
    _Out_ ULONG64 *RepsCompleted);

/* Vector of the next pending SINT, or zero */
ULONG
NTAPI
ReactVSynicTakePending(
    _In_ PREACTV_VP Vp);

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVSynicHypercall(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *Status,
    _Out_ ULONG64 *RepsCompleted);

/* Root VA to host pointer through the given CR3 */
PVOID
NTAPI
ReactVRootTranslate(
    _In_ ULONG64 Cr3,
    _In_ ULONG64 Va,
    _In_ ULONG64 Length);

ULONG
NTAPI
ReactVGuestPage(
    _In_ PREACTV_PARTITION Partition,
    _In_ ULONG64 Gpa,
    _Out_ PULONG64 Address);

/* Pointer to hypercall input for any calling convention. NULL if unmapped or too short. */
_Must_inspect_result_
PVOID
NTAPI
ReactVHypercallInput(
    _In_ PREACTV_HYPERCALL Call,
    _In_ ULONG64 Length);

/* Pointer to hypercall output. Fast call output follows the input registers, hence InputLength. */
_Must_inspect_result_
PVOID
NTAPI
ReactVHypercallOutput(
    _In_ PREACTV_HYPERCALL Call,
    _In_ ULONG64 InputLength,
    _In_ ULONG64 Length);

/*
 * Root GPA to host pointer. The root has no NPT and physical memory is
 * identity mapped in the core. NULL if outside the memory window.
 */
_Must_inspect_result_
PVOID
NTAPI
ReactVRootPointer(
    _In_ ULONG64 Gpa,
    _In_ ULONG64 Length);

/* child.c */

/* Runs a VP until an exit the root must handle */
ULONG64
NTAPI
ReactVChildDispatch(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp,
    _Out_ ULONG64 *ExitReason,
    _Out_ PBOOLEAN Message);

VOID
NTAPI
ReactVChildBuildVmcb(
    _In_ PREACTV_PARTITION Partition,
    _In_ PREACTV_VP Vp);

ULONG64
NTAPI
ReactVChildReadRegister(
    _In_ PREACTV_VP Vp,
    _In_ ULONG Name,
    _Out_ PHV_REGISTER_VALUE Value);

ULONG64
NTAPI
ReactVChildWriteRegister(
    _In_ PREACTV_VP Vp,
    _In_ ULONG Name,
    _In_ PHV_REGISTER_VALUE Value);

/* arch/amd64/svm.S */

/* vmrun a child until #VMEXIT. Host segment/descriptor state is kept in HostVmcb meanwhile. */
VOID
NTAPI
ReactVSvmRunChild(
    _In_ ULONG64 Vmcb,
    _Inout_ PREACTV_GUEST_REGISTERS Registers,
    _In_ ULONG64 HostVmcb);

/* FPU/SSE save and restore */
VOID
NTAPI
ReactVSvmSaveFpu(
    _Out_ PVOID Area);

VOID
NTAPI
ReactVSvmRestoreFpu(
    _In_ PVOID Area);

#endif /* _REACTV_CORE_H_ */
