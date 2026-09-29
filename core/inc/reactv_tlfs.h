/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hypervisor TLFS interface definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_TLFS_H_
#define _REACTV_TLFS_H_

/* Names and values follow the TLFS exactly, including the CPUID leaves */

/* CPUID *********************************************************************/

#define HV_CPUID_FUNCTION_VENDOR_AND_MAX_FUNCTION   0x40000000
#define HV_CPUID_FUNCTION_INTERFACE                 0x40000001
#define HV_CPUID_FUNCTION_VERSION                   0x40000002
#define HV_CPUID_FUNCTION_FEATURES                  0x40000003
#define HV_CPUID_FUNCTION_ENLIGHTENMENT_INFORMATION 0x40000004
#define HV_CPUID_FUNCTION_IMPLEMENTATION_LIMITS     0x40000005
#define HV_CPUID_FUNCTION_HARDWARE_FEATURES         0x40000006

/* End of the reserved hypervisor CPUID range */
#define HV_CPUID_FUNCTION_RANGE_END                 0x400000FF

/* 'Hv#1' interface signature */
#define HV_INTERFACE_SIGNATURE                      0x31237648

/* CPUID.1:ECX hypervisor present bit */
#define HV_CPUID_HYPERVISOR_PRESENT                 0x80000000

/* CPUID 0x40000003 EAX: partition privileges */
#define HV_PARTITION_PRIVILEGE_ACCESS_VP_RUNTIME    0x00000001
#define HV_PARTITION_PRIVILEGE_ACCESS_TIME_REF_COUNT 0x00000002
#define HV_PARTITION_PRIVILEGE_ACCESS_SYNIC_REGS    0x00000004
#define HV_PARTITION_PRIVILEGE_ACCESS_SYNTHETIC_TIMER_REGS 0x00000008
#define HV_PARTITION_PRIVILEGE_ACCESS_APIC_MSRS     0x00000010
#define HV_PARTITION_PRIVILEGE_ACCESS_HYPERCALL_MSRS 0x00000020
#define HV_PARTITION_PRIVILEGE_ACCESS_VP_INDEX      0x00000040
#define HV_PARTITION_PRIVILEGE_ACCESS_RESET_REG     0x00000080
#define HV_PARTITION_PRIVILEGE_ACCESS_FREQUENCY_REGS 0x00000800

/* CPUID 0x40000004 EAX: enlightenment recommendations */
#define HV_ENLIGHTENMENT_USE_APIC_MSRS              0x00000008

/* CPUID 0x40000004 EBX: spinlock retry count, all ones means never notify */
#define HV_ENLIGHTENMENT_NEVER_NOTIFY               0xFFFFFFFF

/* CPUID 0x40000003 EBX: partition privileges */
#define HV_PARTITION_PRIVILEGE_CREATE_PARTITIONS    0x00000001
#define HV_PARTITION_PRIVILEGE_ACCESS_PARTITION_ID  0x00000002
#define HV_PARTITION_PRIVILEGE_ACCESS_MEMORY_POOL   0x00000004
#define HV_PARTITION_PRIVILEGE_POST_MESSAGES        0x00000010
#define HV_PARTITION_PRIVILEGE_SIGNAL_EVENTS        0x00000020
#define HV_PARTITION_PRIVILEGE_CPU_MANAGEMENT       0x00001000

/*
 * CPUID 0x40000006 EAX: hardware features in use. The platform will not create
 * a partition without unrestricted guest, since a guest starts in real mode.
 */
#define HV_HARDWARE_FEATURE_UNRESTRICTED_GUEST      0x00020000

/* Synthetic MSRs ************************************************************/

#define HV_X64_MSR_GUEST_OS_ID                      0x40000000
#define HV_X64_MSR_HYPERCALL                        0x40000001
#define HV_X64_MSR_VP_INDEX                         0x40000002
#define HV_X64_MSR_RESET                            0x40000003
#define HV_X64_MSR_TIME_REF_COUNT                   0x40000020
#define HV_X64_MSR_REFERENCE_TSC                    0x40000021

/*
 * TSC and APIC frequency in Hz. A root scheduler reads the TSC frequency to size
 * time slices even without the frequency privilege, so this must not #GP.
 */
#define HV_X64_MSR_TSC_FREQUENCY                    0x40000022
#define HV_X64_MSR_APIC_FREQUENCY                   0x40000023

/* Synthetic APIC and SynIC MSRs, per VP */
#define HV_X64_MSR_EOI                              0x40000070
#define HV_X64_MSR_ICR                              0x40000071
#define HV_X64_MSR_TPR                              0x40000072
#define HV_X64_MSR_VP_ASSIST_PAGE                   0x40000073
#define HV_X64_MSR_SCONTROL                         0x40000080
#define HV_X64_MSR_SVERSION                         0x40000081
#define HV_X64_MSR_SIEFP                            0x40000082
#define HV_X64_MSR_SIMP                             0x40000083
#define HV_X64_MSR_EOM                              0x40000084
#define HV_X64_MSR_SIRBP                            0x40000085
#define HV_X64_MSR_SINT0                            0x40000090
#define HV_X64_MSR_SINT15                           0x4000009F

/* Four synthetic timers, a config and count MSR each */
#define HV_X64_MSR_STIMER0_CONFIG                   0x400000B0
#define HV_X64_MSR_STIMER3_COUNT                    0x400000B7

#define HV_SYNIC_SINT_COUNT                         16
#define HV_SYNIC_STIMER_COUNT                       4

/* HV_X64_MSR_STIMER0_CONFIG through 3 */
#define HV_STIMER_ENABLE                            0x0000000000000001ULL
#define HV_STIMER_PERIODIC                          0x0000000000000002ULL
#define HV_STIMER_LAZY                              0x0000000000000004ULL
#define HV_STIMER_AUTO_ENABLE                       0x0000000000000008ULL
#define HV_STIMER_SINT_SHIFT                        16
#define HV_STIMER_SINT_MASK                         0x0F

/*
 * Minimum timer period in 100ns units, shorter requests are clamped. Timers are
 * only checked on a VP exit, so anything shorter would always be late.
 */
#define HV_STIMER_MINIMUM_PERIOD                    10000

/* SynIC version reported in SVERSION */
#define HV_SYNIC_VERSION                            1

/* HV_X64_MSR_SCONTROL */
#define HV_SYNIC_CONTROL_ENABLE                     0x0000000000000001ULL

/* HV_X64_MSR_SIEFP, HV_X64_MSR_SIMP and HV_X64_MSR_SIRBP */
#define HV_SYNIC_PAGE_ENABLE                        0x0000000000000001ULL
#define HV_SYNIC_PAGE_MASK                          0xFFFFFFFFFFFFF000ULL

/* HV_X64_MSR_SINT0 through 15 */
#define HV_SYNIC_SINT_VECTOR_MASK                   0x00000000000000FFULL
#define HV_SYNIC_SINT_MASKED                        0x0000000000010000ULL
#define HV_SYNIC_SINT_AUTO_EOI                      0x0000000000020000ULL
#define HV_SYNIC_SINT_POLLING                       0x0000000000040000ULL

/* Deliver the SINT as a message on the hypervisor SINT instead of on its own vector */
#define HV_SYNIC_SINT_PROXY                         0x0000000000080000ULL

/* SINT used for hypervisor messages */
#define HV_SYNIC_HYPERVISOR_SINT                    0

/* Not in the TLFS: posted to the root when a proxied SINT fires, payload is the SINT index */
#define ReactVMessageSynicEvent                     0x80000061

/* SINT reset value */
#define HV_SYNIC_SINT_RESET                         HV_SYNIC_SINT_MASKED

/* Whole synthetic MSR range */
#define HV_X64_MSR_RANGE_BASE                       0x40000000
#define HV_X64_MSR_RANGE_END                        0x40001FFF

/* HV_X64_MSR_HYPERCALL */
#define HV_X64_MSR_HYPERCALL_ENABLE                 0x0000000000000001ULL
#define HV_X64_MSR_HYPERCALL_PAGE_MASK              0xFFFFFFFFFFFFF000ULL

/* Hypercalls ****************************************************************/

/*
 * Input and output are GVAs of the caller. Set for user mode hypercalls; the
 * core walks the caller's page tables to find them.
 */
#define HV_HYPERCALL_VIRTUAL_ADDRESSES              0x0000000040000000ULL

/* Input in RCX */
#define HV_HYPERCALL_CODE_MASK                      0x000000000000FFFFULL
#define HV_HYPERCALL_FAST                           0x0000000000010000ULL
/* Variable header size, in 8 byte units */
#define HV_HYPERCALL_HEADER_SHIFT                   17
#define HV_HYPERCALL_HEADER_MASK                    0x3FF
#define HV_HYPERCALL_HEADER_UNIT                    8

#define HV_HYPERCALL_REP_COUNT_SHIFT                32
#define HV_HYPERCALL_REP_COUNT_MASK                 0x00000FFF00000000ULL
#define HV_HYPERCALL_REP_START_SHIFT                48
#define HV_HYPERCALL_REP_START_MASK                 0x0FFF000000000000ULL

/* Result in RAX */
#define HV_HYPERCALL_STATUS_MASK                    0x000000000000FFFFULL
#define HV_HYPERCALL_REPS_COMPLETED_SHIFT           32

#define HV_STATUS_SUCCESS                           0x0000
#define HV_STATUS_INVALID_HYPERCALL_CODE            0x0002
#define HV_STATUS_INVALID_HYPERCALL_INPUT           0x0003
#define HV_STATUS_INVALID_ALIGNMENT                 0x0004
#define HV_STATUS_INVALID_PARAMETER                 0x0005
#define HV_STATUS_ACCESS_DENIED                     0x0006
#define HV_STATUS_OPERATION_DENIED                  0x0008

#define HV_STATUS_INSUFFICIENT_MEMORY               0x000B
#define HV_STATUS_INVALID_PARTITION_ID              0x000D
#define HV_STATUS_INVALID_VP_INDEX                  0x000E
#define HV_STATUS_INVALID_PORT_ID                   0x0011
#define HV_STATUS_INVALID_CONNECTION_ID             0x0012
#define HV_STATUS_INVALID_PARTITION_STATE           0x0015
#define HV_STATUS_NO_RESOURCES                      0x001D
#define HV_STATUS_INSUFFICIENT_BUFFERS              0x0033
#define HV_STATUS_INVALID_REGISTER_VALUE            0x0050
#define HV_STATUS_INVALID_SYNIC_STATE               0x0080
#define HV_STATUS_OPERATION_FAILED                  0x0071

#define HvCallSwitchVirtualAddressSpace             0x0001
#define HvCallFlushVirtualAddressSpace              0x0002
#define HvCallFlushVirtualAddressList               0x0003
#define HvCallNotifyLongSpinWait                    0x0008
#define HvCallCreatePartition                       0x0040
#define HvCallInitializePartition                   0x0041
#define HvCallFinalizePartition                     0x0042
#define HvCallDeletePartition                       0x0043
#define HvCallGetPartitionProperty                  0x0044
#define HvCallSetPartitionProperty                  0x0045
#define HvCallGetPartitionId                        0x0046
#define HvCallGetNextChildPartition                 0x0047
#define HvCallDepositMemory                         0x0048
#define HvCallWithdrawMemory                        0x0049
#define HvCallGetMemoryBalance                      0x004A
#define HvCallMapGpaPages                           0x004B
#define HvCallUnmapGpaPages                         0x004C
#define HvCallInstallIntercept                      0x004D

/* Message payload size, excluding the 16 byte header */
#define HV_MESSAGE_PAYLOAD_SIZE                     240

/*
 * Ports and connections. A port belongs to one partition and targets one of its
 * VPs; another partition reaches it through a connection ID.
 */
#define HvCallCreatePort                            0x0095
#define HvCallConnectPort                           0x0096
#define HvCallDeletePort                            0x0058
#define HvCallDisconnectPort                        0x005B
#define HvCallGetPortProperty                       0x005A
#define HvCallNotifyPortRingEmpty                   0x008B
#define HvCallSetPortProperty                       0x0070

#define HvPortTypeMessage                           1
#define HvPortTypeEvent                             2
#define HvPortTypeMonitor                           3

typedef struct _HV_INPUT_CREATE_PORT
{
    ULONG64 PortPartitionId;
    ULONG PortId;
    UCHAR Flags[4];
    ULONG64 ConnectionPartitionId;
    ULONG PortType;
    ULONG Reserved;
    ULONG TargetSint;
    ULONG TargetVp;
    USHORT BaseFlagNumber;
    USHORT FlagCount;
    ULONG Reserved2;
    ULONG64 MonitorAddress;
} HV_INPUT_CREATE_PORT, *PHV_INPUT_CREATE_PORT;

C_ASSERT(FIELD_OFFSET(HV_INPUT_CREATE_PORT, ConnectionPartitionId) == 0x10);
C_ASSERT(FIELD_OFFSET(HV_INPUT_CREATE_PORT, PortType) == 0x18);
C_ASSERT(sizeof(HV_INPUT_CREATE_PORT) == 0x38);

typedef struct _HV_INPUT_CONNECT_PORT
{
    ULONG64 ConnectionPartitionId;
    ULONG ConnectionId;
    UCHAR Flags[4];
    ULONG64 PortPartitionId;
    ULONG PortId;
    ULONG Reserved;
    ULONG64 ConnectionInfo[4];
} HV_INPUT_CONNECT_PORT, *PHV_INPUT_CONNECT_PORT;

C_ASSERT(FIELD_OFFSET(HV_INPUT_CONNECT_PORT, PortId) == 0x18);
C_ASSERT(FIELD_OFFSET(HV_INPUT_CONNECT_PORT, ConnectionInfo) == 0x20);

/* Port or connection to delete, passed in registers */
typedef struct _HV_INPUT_NAME_PORT
{
    ULONG64 PartitionId;
    ULONG Id;
    ULONG Reserved;
} HV_INPUT_NAME_PORT, *PHV_INPUT_NAME_PORT;

typedef struct _HV_INPUT_GET_PORT_PROPERTY
{
    ULONG64 PartitionId;
    ULONG PortId;
    ULONG Reserved;
    ULONG PropertyCode;
    ULONG Reserved2;
} HV_INPUT_GET_PORT_PROPERTY, *PHV_INPUT_GET_PORT_PROPERTY;

C_ASSERT(FIELD_OFFSET(HV_INPUT_GET_PORT_PROPERTY, PropertyCode) == 0x10);

/*
 * Not in the TLFS: number of items the core has queued on a port. The root waits
 * for all of them to be delivered before deleting the port, so it must be answered.
 */
#define ReactVPortPropertyTaken                     8

typedef struct _HV_INPUT_SET_PORT_PROPERTY
{
    ULONG64 PartitionId;
    ULONG PortId;
    ULONG Reserved;
    ULONG64 PropertyCode;
    ULONG64 PropertyValue;
} HV_INPUT_SET_PORT_PROPERTY, *PHV_INPUT_SET_PORT_PROPERTY;

/* HvCallPostMessage input */
typedef struct _HV_INPUT_POST_MESSAGE
{
    ULONG ConnectionId;
    ULONG Reserved;
    ULONG MessageType;
    ULONG PayloadSize;
    UCHAR Payload[HV_MESSAGE_PAYLOAD_SIZE];
} HV_INPUT_POST_MESSAGE, *PHV_INPUT_POST_MESSAGE;

C_ASSERT(FIELD_OFFSET(HV_INPUT_POST_MESSAGE, Payload) == 0x10);

typedef struct _HV_INPUT_SIGNAL_EVENT
{
    ULONG ConnectionId;
    USHORT FlagNumber;
    USHORT Reserved;
} HV_INPUT_SIGNAL_EVENT, *PHV_INPUT_SIGNAL_EVENT;
#define HvCallCreateVp                              0x004E
#define HvCallDeleteVp                              0x004F
#define HvCallGetVpRegisters                        0x0050
#define HvCallSetVpRegisters                        0x0051
#define HvCallReadGpa                               0x0053
#define HvCallWriteGpa                              0x0054
#define HvCallSetSystemProperty                     0x006F
#define HvCallTranslateVirtualAddressEx             0x00AC
#define HvCallGetPartitionPropertyEx                0x0101
#define HvCallGetVpState                            0x00E3
#define HvCallSetVpState                            0x00E4

/* HvCallGetVpState/SetVpState state type; XSAVE state is 0 */
#define HvGetSetVpStateXsave                         0

/* XSAVE area: legacy region plus XSAVE header */
#define HV_XSAVE_LEGACY_SIZE                         512
#define HV_XSAVE_HEADER_SIZE                         64
#define HV_XSAVE_SIZE                                (HV_XSAVE_LEGACY_SIZE + \
                                                      HV_XSAVE_HEADER_SIZE)

/* x87 and SSE, the only states the core keeps */
#define HV_XSAVE_LEGACY_STATES                       3

typedef struct _HV_INPUT_ACCESS_VP_STATE
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    UCHAR StateType;
    UCHAR Reserved[3];
    ULONG64 Descriptor[3];
} HV_INPUT_ACCESS_VP_STATE, *PHV_INPUT_ACCESS_VP_STATE;

C_ASSERT(FIELD_OFFSET(HV_INPUT_ACCESS_VP_STATE, Descriptor) == 0x10);
C_ASSERT(sizeof(HV_INPUT_ACCESS_VP_STATE) == 0x28);

/* GVA translation and GPA access for the root. Access size is capped at what fits beside the result code. */
#define HV_ACCESS_GPA_MAX_BYTES                     16

#define HV_TRANSLATE_GVA_VALIDATE_READ               0x0001
#define HV_TRANSLATE_GVA_VALIDATE_WRITE              0x0002
#define HV_TRANSLATE_GVA_VALIDATE_EXECUTE            0x0004
#define HV_TRANSLATE_GVA_PRIVILEGE_EXEMPT            0x0008
#define HV_TRANSLATE_GVA_SET_PAGE_TABLE_BITS         0x0010

#define HvTranslateGvaSuccess                        0
#define HvTranslateGvaPageNotPresent                 1
#define HvTranslateGvaPrivilegeViolation             2
#define HvTranslateGvaInvalidPageTableFlags          3
#define HvTranslateGvaGpaUnmapped                    4
#define HvTranslateGvaGpaNoReadAccess                5
#define HvTranslateGvaGpaNoWriteAccess               6

#define HvAccessGpaSuccess                           0
#define HvAccessGpaUnmapped                          1
#define HvAccessGpaReadIntercept                     2
#define HvAccessGpaWriteIntercept                    3

typedef struct _HV_INPUT_TRANSLATE_VIRTUAL_ADDRESS
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    ULONG Reserved;
    ULONG64 ControlFlags;
    ULONG64 GvaPage;
} HV_INPUT_TRANSLATE_VIRTUAL_ADDRESS, *PHV_INPUT_TRANSLATE_VIRTUAL_ADDRESS;

typedef struct _HV_OUTPUT_TRANSLATE_VIRTUAL_ADDRESS
{
    ULONG ResultCode;
    ULONG CacheType;
    /* Ex form: event the walk would have raised */
    ULONG64 Event[5];
    ULONG64 GpaPage;
} HV_OUTPUT_TRANSLATE_VIRTUAL_ADDRESS, *PHV_OUTPUT_TRANSLATE_VIRTUAL_ADDRESS;

C_ASSERT(FIELD_OFFSET(HV_OUTPUT_TRANSLATE_VIRTUAL_ADDRESS, GpaPage) == 0x30);

typedef struct _HV_INPUT_ACCESS_GPA
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    ULONG ByteCount;
    ULONG64 BaseGpa;
    ULONG64 ControlFlags;
    UCHAR Data[HV_ACCESS_GPA_MAX_BYTES];
} HV_INPUT_ACCESS_GPA, *PHV_INPUT_ACCESS_GPA;

C_ASSERT(FIELD_OFFSET(HV_INPUT_ACCESS_GPA, Data) == 0x20);

typedef struct _HV_OUTPUT_ACCESS_GPA
{
    ULONG ResultCode;
    ULONG Reserved;
    UCHAR Data[HV_ACCESS_GPA_MAX_BYTES];
} HV_OUTPUT_ACCESS_GPA, *PHV_OUTPUT_ACCESS_GPA;
#define HvCallPostMessage                           0x005C
#define HvCallSignalEvent                           0x005D
#define HvCallGetSystemInformation                  0x007B
#define HvCallStartVirtualProcessor                 0x0099
#define HvCallMapStatsPage                          0x006C
#define HvCallUnmapStatsPage                        0x006D
#define HvCallMapVpStatePage                        0x00E1
#define HvCallUnmapVpStatePage                      0x00E2
#define HvCallPrecommitGpaPages                     0x00BE
#define HvCallUncommitGpaPages                      0x00BF
#define HvCallDispatchVp                            0x00C2

/* Post a message or signal an event directly to a VP, bypassing ports. The root uses these for its own guests. */
#define HvCallSignalEventDirect                     0x00C0
#define HvCallPostMessageDirect                     0x00C1

typedef struct _HV_INPUT_POST_MESSAGE_DIRECT
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    UCHAR Vtl;
    UCHAR Reserved[3];
    ULONG SintIndex;
    UCHAR Message[256];
} HV_INPUT_POST_MESSAGE_DIRECT, *PHV_INPUT_POST_MESSAGE_DIRECT;

C_ASSERT(FIELD_OFFSET(HV_INPUT_POST_MESSAGE_DIRECT, SintIndex) == 0x10);
C_ASSERT(FIELD_OFFSET(HV_INPUT_POST_MESSAGE_DIRECT, Message) == 0x14);

typedef struct _HV_INPUT_SIGNAL_EVENT_DIRECT
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    UCHAR Vtl;
    UCHAR SintIndex;
    USHORT FlagNumber;
} HV_INPUT_SIGNAL_EVENT_DIRECT, *PHV_INPUT_SIGNAL_EVENT_DIRECT;

C_ASSERT(sizeof(HV_INPUT_SIGNAL_EVENT_DIRECT) == 16);

typedef struct _HV_OUTPUT_SIGNAL_EVENT_DIRECT
{
    UCHAR NewlySignalled;
    UCHAR Reserved[7];
} HV_OUTPUT_SIGNAL_EVENT_DIRECT, *PHV_OUTPUT_SIGNAL_EVENT_DIRECT;

/* Not named in the TLFS: returns the current logical processor index */
#define ReactVCallLogicalProcessorIndex             0x008A

/* HvCallGetSystemInformation class, the only one the root queries */
#define HV_SYSTEM_INFORMATION_SCHEDULER_TYPE        15

/* Scheduler types. Only the root scheduler is implemented: the root dispatches VPs on its own threads. */
#define HV_SCHEDULER_TYPE_CLASSIC_NO_SMT            1
#define HV_SCHEDULER_TYPE_CLASSIC_SMT               2
#define HV_SCHEDULER_TYPE_CORE                      3
#define HV_SCHEDULER_TYPE_ROOT                      4

/* PARTITIONS ****************************************************************/

/* Caller's own partition */
#define HV_PARTITION_ID_SELF                        ((ULONG64)-1)
#define HV_PARTITION_ID_INVALID                     ((ULONG64)0)

/* Partition ID for calls made through a doorbell page, which identifies the partition */
#define HV_PARTITION_ID_DOORBELL                    ((ULONG64)-2)

/* HvCallCreatePartition flags */
#define HV_PARTITION_CREATION_FLAG_SMT_CHILDREN_ENABLED    0x00000004
#define HV_PARTITION_CREATION_FLAG_GPA_SUPER_PAGES_ENABLED 0x00000010
#define HV_PARTITION_CREATION_FLAG_EXO_PARTITION           0x00000400

/* Property class in the high 16 bits */
#define HvPartitionPropertyPrivilegeFlags           0x00010000
#define HvPartitionPropertySyntheticProcFeatures    0x00010001
#define HvPartitionPropertyCpuReserve               0x00020001
#define HvPartitionPropertyCpuCap                   0x00020002
#define HvPartitionPropertyCpuWeight                0x00020003
#define HvPartitionPropertyProcessorCount           0x00030005
#define HvPartitionPropertyGpaSpaceOptions          0x00050005
#define HvPartitionPropertyDoorbellPage             0x00050012
#define HvPartitionPropertyProcessorVendor          0x00060000
#define HvPartitionPropertyProcessorFeatures        0x00060001
#define HvPartitionPropertyProcessorXsaveFeatures   0x00060002
#define HvPartitionPropertyProcessorClFlushSize     0x00060003
#define HvPartitionPropertyPhysicalAddressWidth     0x00060006
#define HvPartitionPropertyXsaveStates              0x00060007
#define HvPartitionPropertyMaxXsaveDataSize         0x00060008
#define HvPartitionPropertyProcessorClockFrequency  0x00060009
#define HvPartitionPropertyProcessorFeaturesBank0   0x0006000A
#define HvPartitionPropertyProcessorFeaturesBank1   0x0006000B

/* HvPartitionPropertyGpaSpaceOptions bits */
#define HV_GPA_SPACE_ENABLED                        0x00000001
#define HV_GPA_SPACE_LARGE_PAGES                    0x00000002
#define HV_GPA_SPACE_ACCESS_TRACKING                0x00000008

/* GPA width, child partitions only */
#define HvPartitionPropertyGpaAddressWidth          0x00060014

/* HvPartitionPropertyProcessorVendor values */
#define HV_PROCESSOR_VENDOR_AMD                     0
#define HV_PROCESSOR_VENDOR_INTEL                   1
#define HV_PROCESSOR_VENDOR_HYGON                   2

typedef struct _HV_INPUT_CREATE_PARTITION
{
    ULONG64 Flags;
    ULONG64 ProximityDomainInfo;
    ULONG CompatibilityVersion;
    ULONG Reserved;
    ULONG64 IsolationProperties[2];
    ULONG64 IsolationPolicy;
    /* Ignored */
    ULONG64 Reserved2;
} HV_INPUT_CREATE_PARTITION, *PHV_INPUT_CREATE_PARTITION;

C_ASSERT(sizeof(HV_INPUT_CREATE_PARTITION) == 0x38);

typedef struct _HV_INPUT_PARTITION_ID
{
    ULONG64 PartitionId;
} HV_INPUT_PARTITION_ID, *PHV_INPUT_PARTITION_ID;

typedef struct _HV_INPUT_GET_PARTITION_PROPERTY
{
    ULONG64 PartitionId;
    ULONG PropertyCode;
    ULONG Reserved;
} HV_INPUT_GET_PARTITION_PROPERTY, *PHV_INPUT_GET_PARTITION_PROPERTY;

typedef struct _HV_INPUT_SET_PARTITION_PROPERTY
{
    ULONG64 PartitionId;
    ULONG PropertyCode;
    ULONG Reserved;
    ULONG64 PropertyValue;
} HV_INPUT_SET_PARTITION_PROPERTY, *PHV_INPUT_SET_PARTITION_PROPERTY;

/* Ex form: takes an extra argument and returns up to HV_PARTITION_PROPERTY_EX_OUTPUT bytes */
typedef struct _HV_INPUT_GET_PARTITION_PROPERTY_EX
{
    ULONG64 PartitionId;
    ULONG PropertyCode;
    ULONG Reserved;
    ULONG64 Argument;
} HV_INPUT_GET_PARTITION_PROPERTY_EX, *PHV_INPUT_GET_PARTITION_PROPERTY_EX;

C_ASSERT(FIELD_OFFSET(HV_INPUT_GET_PARTITION_PROPERTY_EX, Argument) == 0x10);

#define HV_PARTITION_PROPERTY_EX_OUTPUT 0x0FE8

/* System wide property, value size depends on the property ID */
#define HV_SYSTEM_PROPERTY_VALUE_SIZE 32

typedef struct _HV_INPUT_SET_SYSTEM_PROPERTY
{
    ULONG PropertyId;
    ULONG Reserved;
    UCHAR PropertyValue[HV_SYSTEM_PROPERTY_VALUE_SIZE];
} HV_INPUT_SET_SYSTEM_PROPERTY, *PHV_INPUT_SET_SYSTEM_PROPERTY;

C_ASSERT(FIELD_OFFSET(HV_INPUT_SET_SYSTEM_PROPERTY, PropertyValue) == 8);
C_ASSERT(sizeof(HV_INPUT_SET_SYSTEM_PROPERTY) == 0x28);

/* MEMORY ********************************************************************/

/* HvCallDepositMemory header, followed by one PFN per rep */
typedef struct _HV_INPUT_DEPOSIT_MEMORY
{
    ULONG64 PartitionId;
    ULONG64 GpaPageList[ANYSIZE_ARRAY];
} HV_INPUT_DEPOSIT_MEMORY, *PHV_INPUT_DEPOSIT_MEMORY;

/*
 * HvCallMapStatsPage object types, returns the PFN of the counter page. The
 * partition ID and VP index follow the type.
 */
#define HV_STATS_OBJECT_HYPERVISOR                  0x00000001
#define HV_STATS_OBJECT_LOGICAL_PROCESSOR           0x00000002
#define HV_STATS_OBJECT_PARTITION                   0x00010001
#define HV_STATS_OBJECT_VP                          0x00010002

/* HvCallMapVpStatePage types, returns the PFN of a per VP page shared by root and core */
#define HV_VP_STATE_PAGE_REGISTERS                  0
#define HV_VP_STATE_PAGE_INTERCEPT_MESSAGE          1
#define HV_VP_STATE_PAGE_TYPES                      4

#include <pshpack1.h>
typedef struct _HV_INPUT_MAP_VP_STATE_PAGE
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    USHORT StateType;
    UCHAR Reserved[10];
} HV_INPUT_MAP_VP_STATE_PAGE, *PHV_INPUT_MAP_VP_STATE_PAGE;
#include <poppack.h>

/* HvCallInstallIntercept types. A zero access mask removes the intercept. */
#define HvInterceptTypeX64IoPort                    0x00000000
#define HvInterceptTypeX64Msr                       0x00000001
#define HvInterceptTypeX64Cpuid                     0x00000002
#define HvInterceptTypeException                    0x00000003
#define HvInterceptTypeRegister                     0x00000004
#define HvInterceptTypeMmio                         0x00000005
#define HvInterceptTypeX64GlobalCpuid               0x00000006
#define HvInterceptTypeHypercall                    0x00000008
#define HvInterceptTypeX64MsrIndex                  0x0000000C

#define HV_INTERCEPT_ACCESS_MASK_READ               0x00000001
#define HV_INTERCEPT_ACCESS_MASK_WRITE              0x00000002
#define HV_INTERCEPT_ACCESS_MASK_EXECUTE            0x00000004

typedef struct _HV_INPUT_INSTALL_INTERCEPT
{
    ULONG64 PartitionId;
    ULONG AccessTypeMask;
    ULONG InterceptType;
    ULONG64 InterceptParameter;
} HV_INPUT_INSTALL_INTERCEPT, *PHV_INPUT_INSTALL_INTERCEPT;

C_ASSERT(FIELD_OFFSET(HV_INPUT_INSTALL_INTERCEPT, InterceptType) == 0x0C);
C_ASSERT(FIELD_OFFSET(HV_INPUT_INSTALL_INTERCEPT, InterceptParameter) == 0x10);

typedef struct _HV_INPUT_MAP_STATS_PAGE
{
    ULONG64 StatsType;
    ULONG64 PartitionId;
    ULONG VpIndex;
    ULONG Reserved;
} HV_INPUT_MAP_STATS_PAGE, *PHV_INPUT_MAP_STATS_PAGE;

/* HvCallPrecommitGpaPages: start page in the header, page count in the rep count */
typedef struct _HV_INPUT_PRECOMMIT_GPA_PAGES
{
    ULONG64 PartitionId;
    ULONG Flags;
    ULONG Reserved;
    ULONG64 StartGpaPage;
} HV_INPUT_PRECOMMIT_GPA_PAGES, *PHV_INPUT_PRECOMMIT_GPA_PAGES;

typedef struct _HV_INPUT_UNCOMMIT_GPA_PAGES
{
    ULONG64 PartitionId;
    ULONG64 StartGpaPage;
} HV_INPUT_UNCOMMIT_GPA_PAGES, *PHV_INPUT_UNCOMMIT_GPA_PAGES;

/* HvCallWithdrawMemory input, output is one PFN per rep */
typedef struct _HV_INPUT_WITHDRAW_MEMORY
{
    ULONG64 PartitionId;
    ULONG64 ProximityDomainInfo;
} HV_INPUT_WITHDRAW_MEMORY, *PHV_INPUT_WITHDRAW_MEMORY;

typedef struct _HV_OUTPUT_GET_MEMORY_BALANCE
{
    ULONG64 PagesAvailable;
    ULONG64 PagesInUse;
} HV_OUTPUT_GET_MEMORY_BALANCE, *PHV_OUTPUT_GET_MEMORY_BALANCE;

/* HvCallMapGpaPages access flags */
#define HV_MAP_GPA_READABLE                         0x00000001
#define HV_MAP_GPA_WRITABLE                         0x00000002
#define HV_MAP_GPA_KERNEL_EXECUTABLE                0x00000004
#define HV_MAP_GPA_USER_EXECUTABLE                  0x00000008
#define HV_MAP_GPA_PERMISSIONS_MASK                 0x0000000F

/* Large page hint, ignored since the core maps 4K pages */
#define HV_MAP_GPA_LARGE_PAGE                       0x00008000

/* Either bit means no page list: only the access of already mapped pages changes */
#define HV_MAP_GPA_NO_PAGE_LIST                     0x00030000

/* 40 bit page number */
#define HV_PAGE_NUMBER_MASK                         0x000000FFFFFFFFFFULL

typedef struct _HV_INPUT_MAP_GPA_PAGES
{
    ULONG64 TargetPartitionId;
    ULONG64 TargetGpaBase;
    ULONG MapFlags;
    ULONG Reserved;
    ULONG64 SourcePageList[ANYSIZE_ARRAY];
} HV_INPUT_MAP_GPA_PAGES, *PHV_INPUT_MAP_GPA_PAGES;

typedef struct _HV_INPUT_UNMAP_GPA_PAGES
{
    ULONG64 TargetPartitionId;
    ULONG64 TargetGpaBase;
    ULONG UnmapFlags;
    ULONG Reserved;
} HV_INPUT_UNMAP_GPA_PAGES, *PHV_INPUT_UNMAP_GPA_PAGES;

/* VIRTUAL PROCESSORS ********************************************************/

typedef struct _HV_INPUT_CREATE_VP
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    UCHAR Padding[3];
    UCHAR SubnodeType;
    ULONG64 SubnodeId;
    ULONG64 ProximityDomainInfo;
    ULONG64 Flags;
} HV_INPUT_CREATE_VP, *PHV_INPUT_CREATE_VP;

typedef struct _HV_INPUT_DELETE_VP
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    ULONG Reserved;
} HV_INPUT_DELETE_VP, *PHV_INPUT_DELETE_VP;

/* Register values are 16 bytes regardless of register */
typedef struct _HV_REGISTER_VALUE
{
    ULONG64 Low;
    ULONG64 High;
} HV_REGISTER_VALUE, *PHV_REGISTER_VALUE;

typedef struct _HV_REGISTER_ASSOC
{
    ULONG Name;
    ULONG Reserved1;
    ULONG64 Reserved2;
    HV_REGISTER_VALUE Value;
} HV_REGISTER_ASSOC, *PHV_REGISTER_ASSOC;

typedef struct _HV_INPUT_VP_REGISTERS
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    UCHAR TargetVtl;
    UCHAR Padding[3];
} HV_INPUT_VP_REGISTERS, *PHV_INPUT_VP_REGISTERS;

/* Register names, per the TLFS */

/* Suspend registers. A root scheduler holds a VP by setting a suspend and releases it by clearing it. */
#define HvRegisterExplicitSuspend                   0x00000000
#define HvRegisterInterceptSuspend                  0x00000001
#define HvRegisterEmulationHints                    0x00000002
#define HvRegisterDispatchSuspend                   0x00000003
#define HvRegisterInternalActivityState             0x00000004

/*
 * HvRegisterInternalActivityState bits. Every VP but the BSP starts in startup
 * suspend; the root clears it to start a VP without a guest SIPI.
 */
#define HV_ACTIVITY_STARTUP_SUSPEND                 0x0000000000000001ULL
#define HV_ACTIVITY_HALT_SUSPEND                    0x0000000000000002ULL
#define HV_ACTIVITY_IDLE_SUSPEND                    0x0000000000000004ULL

#define HV_SUSPEND_SUSPENDED                        0x0000000000000001ULL

/* Interrupt injection and deliverability notification registers */
#define HvRegisterPendingInterruption                0x00010002
#define HvRegisterInterruptState                     0x00010003
#define HvRegisterPendingEvent0                      0x00010004
#define HvRegisterPendingEvent1                      0x00010005
#define HvX64RegisterDeliverabilityNotifications     0x00010006

/* HvRegisterPendingInterruption layout, written by the root */
#define HV_PENDING_INTERRUPTION_PENDING             0x00000001
#define HV_PENDING_INTERRUPTION_TYPE_SHIFT          1
#define HV_PENDING_INTERRUPTION_TYPE_MASK           0x00000007
#define HV_PENDING_INTERRUPTION_ERROR_CODE_VALID    0x00000010
#define HV_PENDING_INTERRUPTION_LENGTH_SHIFT        5
#define HV_PENDING_INTERRUPTION_LENGTH_MASK         0x0000000F
#define HV_PENDING_INTERRUPTION_VECTOR_SHIFT        16
#define HV_PENDING_INTERRUPTION_VECTOR_MASK         0x0000FFFF
#define HV_PENDING_INTERRUPTION_ERROR_CODE_SHIFT    32

/* Interruption types, numbered as in VMX */
#define HvX64PendingInterrupt                       0
#define HvX64PendingNmi                             2
#define HvX64PendingException                       3
#define HvX64PendingSoftwareInterrupt               4

/* HvRegisterInterruptState bits */
#define HV_INTERRUPT_STATE_SHADOW                   0x00000001
#define HV_INTERRUPT_STATE_NMI_MASKED               0x00000002

/* HvX64RegisterDeliverabilityNotifications bits */
#define HV_DELIVERABILITY_NMI                       0x00000001
#define HV_DELIVERABILITY_INTERRUPT                 0x00000002
#define HV_DELIVERABILITY_PRIORITY_SHIFT            2
#define HV_DELIVERABILITY_PRIORITY_MASK             0x0000000F

/* DeliverableType in HV_X64_INTERRUPTION_DELIVERABLE_MESSAGE */
#define HV_DELIVERABLE_TYPE_NMI                     0
#define HV_DELIVERABLE_TYPE_INTERRUPT               1

#define HvRegisterVpRuntime                         0x00090000
#define HvRegisterGuestOsId                         0x00090002
#define HvRegisterVpIndex                           0x00090003

/* Pending root signals for a VP, zero until the SynIC has any */
#define HvRegisterVpRootSignalCount                 0x00090014

/* FP and vector registers. XMM0-5 are also in the register page. */
#define HvX64RegisterXmm0                           0x00030000
#define HvX64RegisterXmm15                          0x0003000F
#define HvX64RegisterFpMmx0                         0x00030010
#define HvX64RegisterFpMmx7                         0x00030017
#define HvX64RegisterFpControlStatus                0x00030018
#define HvX64RegisterXmmControlStatus               0x00030019

#define HvX64RegisterRax                            0x00020000
#define HvX64RegisterRcx                            0x00020001
#define HvX64RegisterRdx                            0x00020002
#define HvX64RegisterRbx                            0x00020003
#define HvX64RegisterRsp                            0x00020004
#define HvX64RegisterRbp                            0x00020005
#define HvX64RegisterRsi                            0x00020006
#define HvX64RegisterRdi                            0x00020007
#define HvX64RegisterR8                             0x00020008
#define HvX64RegisterR9                             0x00020009
#define HvX64RegisterR10                            0x0002000A
#define HvX64RegisterR11                            0x0002000B
#define HvX64RegisterR12                            0x0002000C
#define HvX64RegisterR13                            0x0002000D
#define HvX64RegisterR14                            0x0002000E
#define HvX64RegisterR15                            0x0002000F
#define HvX64RegisterRip                            0x00020010
#define HvX64RegisterRflags                         0x00020011

#define HvX64RegisterCr0                            0x00040000

/* CR0/3/4/8 aliases; 0x1000 marks registers carried in the register page */
#define HvX64RegisterCr0Paged                       0x00041000
#define HvX64RegisterCr3Paged                       0x00041002
#define HvX64RegisterCr4Paged                       0x00041003
#define HvX64RegisterCr8Paged                       0x00041004

#define HvX64RegisterCr2                            0x00040001
#define HvX64RegisterCr3                            0x00040002
#define HvX64RegisterCr4                            0x00040003
#define HvX64RegisterCr8                            0x00040004
#define HvX64RegisterXfem                           0x00040005

#define HvX64RegisterEs                             0x00060000
#define HvX64RegisterCs                             0x00060001
#define HvX64RegisterSs                             0x00060002
#define HvX64RegisterDs                             0x00060003
#define HvX64RegisterFs                             0x00060004
#define HvX64RegisterGs                             0x00060005
#define HvX64RegisterLdtr                           0x00060006
#define HvX64RegisterTr                             0x00060007
#define HvX64RegisterIdtr                           0x00070000
#define HvX64RegisterGdtr                           0x00070001

#define HvX64RegisterEfer                           0x00080001
#define HvX64RegisterKernelGsBase                   0x00080002
#define HvX64RegisterApicBase                       0x00080003
#define HvX64RegisterPat                            0x00080004
#define HvX64RegisterSysenterCs                     0x00080005
#define HvX64RegisterSysenterEip                    0x00080006
#define HvX64RegisterSysenterEsp                    0x00080007
#define HvX64RegisterStar                           0x00080008
#define HvX64RegisterLstar                          0x00080009
#define HvX64RegisterCstar                          0x0008000A
#define HvX64RegisterSfmask                         0x0008000B

/* Run a VP on the calling processor, returns when the VP exits */
typedef struct _HV_INPUT_DISPATCH_VP
{
    ULONG64 PartitionId;
    ULONG VpIndex;
    ULONG Flags;
    ULONG64 TimeSlice;
    ULONG64 SpecCtrl;
} HV_INPUT_DISPATCH_VP, *PHV_INPUT_DISPATCH_VP;

typedef struct _HV_OUTPUT_DISPATCH_VP
{
    ULONG DispatchState;
    ULONG DispatchEvent;
    ULONG64 Reserved;
} HV_OUTPUT_DISPATCH_VP, *PHV_OUTPUT_DISPATCH_VP;

C_ASSERT(sizeof(HV_INPUT_DISPATCH_VP) == 32);
C_ASSERT(sizeof(HV_OUTPUT_DISPATCH_VP) == 16);

/* HV_OUTPUT_DISPATCH_VP DispatchState */
#define HV_VP_DISPATCH_STATE_INVALID                0
#define HV_VP_DISPATCH_STATE_BLOCKED                1
#define HV_VP_DISPATCH_STATE_READY                  2

/* HV_OUTPUT_DISPATCH_VP DispatchEvent, INTERCEPT means a message is pending */
#define HV_VP_DISPATCH_EVENT_INVALID                0
#define HV_VP_DISPATCH_EVENT_SUSPEND                1
#define HV_VP_DISPATCH_EVENT_INTERCEPT              2

/*
 * Not in the TLFS: exit reason and qualification from the core's control block,
 * read as registers by a root scheduler. ReactV names so they are not mistaken
 * for Hyper-V registers.
 */
#define ReactVRegisterInterceptReason               0x7F000000
#define ReactVRegisterInterceptInfo1                0x7F000001
#define ReactVRegisterInterceptInfo2                0x7F000002
#define ReactVRegisterInterceptLength               0x7F000003

/* ReactVRegisterInterceptReason values */
#define ReactVInterceptNone                         0
#define ReactVInterceptHalt                         1
#define ReactVInterceptIoPort                       2
#define ReactVInterceptMemory                       3
#define ReactVInterceptCpuid                        4
#define ReactVInterceptMsr                          5
#define ReactVInterceptShutdown                     6
#define ReactVInterceptException                    7
#define ReactVInterceptHypercall                    8
#define ReactVInterceptInterrupted                  9
#define ReactVInterceptFailed                       10
#define ReactVInterceptDeliverable                  11

/* TLFS segment register format */
typedef struct _HV_X64_SEGMENT_REGISTER
{
    ULONG64 Base;
    ULONG Limit;
    USHORT Selector;
    USHORT Attributes;
} HV_X64_SEGMENT_REGISTER, *PHV_X64_SEGMENT_REGISTER;

typedef struct _HV_X64_TABLE_REGISTER
{
    USHORT Pad[3];
    USHORT Limit;
    ULONG64 Base;
} HV_X64_TABLE_REGISTER, *PHV_X64_TABLE_REGISTER;

/*
 * Per VP register page shared by root and core. The root sets Dirty bits for
 * groups it changed; the core loads those before a run, clears Dirty and writes
 * back the VP state on exit.
 */
#define HV_VP_REGISTER_PAGE_VERSION_1               0

#define HV_REGISTER_PAGE_DIRTY_GP                   0x00000001
#define HV_REGISTER_PAGE_DIRTY_RIP                  0x00000002
#define HV_REGISTER_PAGE_DIRTY_EXTENDED             0x00000004
#define HV_REGISTER_PAGE_DIRTY_SEGMENT              0x00000008
#define HV_REGISTER_PAGE_DIRTY_RFLAGS               0x00000010

/* GP registers in encoding order */
#define HV_REGISTER_PAGE_GP_COUNT                   16

/* Segment slots in the register page */
#define HV_REGISTER_PAGE_SEGMENT_ES                 0
#define HV_REGISTER_PAGE_SEGMENT_CS                 1
#define HV_REGISTER_PAGE_SEGMENT_SS                 2
#define HV_REGISTER_PAGE_SEGMENT_DS                 3
#define HV_REGISTER_PAGE_SEGMENT_FS                 4
#define HV_REGISTER_PAGE_SEGMENT_GS                 5
#define HV_REGISTER_PAGE_SEGMENT_COUNT              6

typedef struct _HV_VP_REGISTER_PAGE
{
    USHORT Version;
    UCHAR IsValid;
    UCHAR Vtl;
    ULONG Dirty;
    ULONG64 Gp[HV_REGISTER_PAGE_GP_COUNT];
    ULONG64 Rip;
    ULONG64 Rflags;
    ULONG64 Reserved0;
    ULONG64 Extended[6][2];
    HV_X64_SEGMENT_REGISTER Segment[HV_REGISTER_PAGE_SEGMENT_COUNT];
    ULONG64 Cr0;
    ULONG64 Cr3;
    ULONG64 Cr4;
    ULONG64 Cr8;
    ULONG64 Efer;
    ULONG64 Dr7;
    ULONG64 PendingInterruption;
    ULONG64 InterruptState;
    ULONG64 Reserved1;
    ULONG64 Xfem;
} HV_VP_REGISTER_PAGE, *PHV_VP_REGISTER_PAGE;

C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Gp) == 0x008);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Rip) == 0x088);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Rflags) == 0x090);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Extended) == 0x0A0);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Segment) == 0x100);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Cr0) == 0x160);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Efer) == 0x180);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Dr7) == 0x188);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, PendingInterruption) == 0x190);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, InterruptState) == 0x198);
C_ASSERT(FIELD_OFFSET(HV_VP_REGISTER_PAGE, Xfem) == 0x1A8);

/* MESSAGES ******************************************************************/

/* Intercept message types */
#define HvMessageTypeUnmappedGpa                    0x80000000
#define HvMessageTypeGpaIntercept                   0x80000001
#define HvMessageTypeUnrecoverableException          0x80000003
#define HvMessageTypeX64IoPortIntercept             0x80010000
#define HvMessageTypeX64MsrIntercept                0x80010001
#define HvMessageTypeX64CpuidIntercept              0x80010002
#define HvMessageTypeX64ExceptionIntercept          0x80010003
#define HvMessageTypeX64ApicEoi                     0x80010004
#define HvMessageTypeX64RegisterIntercept           0x80010006
#define HvMessageTypeX64Halt                        0x80010007
#define HvMessageTypeX64InterruptionDeliverable     0x80010008
#define HvMessageTypeHypercallIntercept             0x80000050
#define HvMessageTypeTimerExpired                    0x80000010

/* HvMessageTypeTimerExpired payload */
typedef struct _HV_TIMER_MESSAGE_PAYLOAD
{
    ULONG TimerIndex;
    ULONG Reserved;
    ULONG64 ExpirationTime;
    ULONG64 DeliveryTime;
} HV_TIMER_MESSAGE_PAYLOAD, *PHV_TIMER_MESSAGE_PAYLOAD;

/* InterceptAccessType values */
#define HV_INTERCEPT_ACCESS_READ                    0
#define HV_INTERCEPT_ACCESS_WRITE                   1
#define HV_INTERCEPT_ACCESS_EXECUTE                 2

/* ExecutionState bits */
#define HV_EXECUTION_STATE_CR0_PE                   0x0004
#define HV_EXECUTION_STATE_CR0_AM                   0x0008
#define HV_EXECUTION_STATE_EFER_LMA                 0x0010

/* MemoryAccessInfo bits */
#define HV_MEMORY_ACCESS_GVA_VALID                  0x01

typedef struct _HV_X64_INTERCEPT_MESSAGE_HEADER
{
    ULONG VpIndex;
    UCHAR InstructionLength;
    UCHAR InterceptAccessType;
    USHORT ExecutionState;
    HV_X64_SEGMENT_REGISTER CsSegment;
    ULONG64 Rip;
    ULONG64 Rflags;
} HV_X64_INTERCEPT_MESSAGE_HEADER, *PHV_X64_INTERCEPT_MESSAGE_HEADER;

typedef struct _HV_X64_MEMORY_INTERCEPT_MESSAGE
{
    HV_X64_INTERCEPT_MESSAGE_HEADER Header;
    ULONG CacheType;
    UCHAR InstructionByteCount;
    UCHAR MemoryAccessInfo;
    USHORT Reserved;
    ULONG64 GuestVirtualAddress;
    ULONG64 GuestPhysicalAddress;
    UCHAR InstructionBytes[16];
} HV_X64_MEMORY_INTERCEPT_MESSAGE, *PHV_X64_MEMORY_INTERCEPT_MESSAGE;

typedef struct _HV_X64_IO_PORT_INTERCEPT_MESSAGE
{
    HV_X64_INTERCEPT_MESSAGE_HEADER Header;
    USHORT PortNumber;
    UCHAR AccessInfo;
    UCHAR InstructionByteCount;
    ULONG Reserved;
    ULONG64 Rax;
    UCHAR InstructionBytes[16];
    HV_X64_SEGMENT_REGISTER DsSegment;
    HV_X64_SEGMENT_REGISTER EsSegment;
    ULONG64 Rcx;
    ULONG64 Rsi;
    ULONG64 Rdi;
} HV_X64_IO_PORT_INTERCEPT_MESSAGE, *PHV_X64_IO_PORT_INTERCEPT_MESSAGE;

/* IO port AccessInfo bits */
#define HV_PORT_ACCESS_SIZE_MASK                    0x07
#define HV_PORT_ACCESS_STRING                       0x08
#define HV_PORT_ACCESS_REP                          0x10

/* DefaultResult is what the core would return, so a root with no override writes it back as is */
typedef struct _HV_X64_CPUID_INTERCEPT_MESSAGE
{
    HV_X64_INTERCEPT_MESSAGE_HEADER Header;
    ULONG64 Rax;
    ULONG64 Rcx;
    ULONG64 Rdx;
    ULONG64 Rbx;
    ULONG64 DefaultResultRax;
    ULONG64 DefaultResultRcx;
    ULONG64 DefaultResultRdx;
    ULONG64 DefaultResultRbx;
} HV_X64_CPUID_INTERCEPT_MESSAGE, *PHV_X64_CPUID_INTERCEPT_MESSAGE;

typedef struct _HV_X64_MSR_INTERCEPT_MESSAGE
{
    HV_X64_INTERCEPT_MESSAGE_HEADER Header;
    ULONG MsrNumber;
    ULONG Reserved;
    ULONG64 Rdx;
    ULONG64 Rax;
} HV_X64_MSR_INTERCEPT_MESSAGE, *PHV_X64_MSR_INTERCEPT_MESSAGE;

/* Hypercall intercept. Registers are the hypercall ABI inputs. */
typedef struct _HV_X64_HYPERCALL_INTERCEPT_MESSAGE
{
    HV_X64_INTERCEPT_MESSAGE_HEADER Header;
    ULONG64 Rax;
    ULONG64 Rbx;
    ULONG64 Rcx;
    ULONG64 Rdx;
    ULONG64 R8;
    ULONG64 Rsi;
    ULONG64 Rdi;
    /* Directly follows the GP registers, no padding */
    ULONG64 Xmm[6][2];
} HV_X64_HYPERCALL_INTERCEPT_MESSAGE, *PHV_X64_HYPERCALL_INTERCEPT_MESSAGE;

C_ASSERT(FIELD_OFFSET(HV_X64_HYPERCALL_INTERCEPT_MESSAGE, Rax) == 0x28);
C_ASSERT(FIELD_OFFSET(HV_X64_HYPERCALL_INTERCEPT_MESSAGE, Xmm) == 0x60);

/* Sent when a pending interruption becomes deliverable */
typedef struct _HV_X64_INTERRUPTION_DELIVERABLE_MESSAGE
{
    HV_X64_INTERCEPT_MESSAGE_HEADER Header;
    ULONG DeliverableType;
    ULONG Reserved;
} HV_X64_INTERRUPTION_DELIVERABLE_MESSAGE, *PHV_X64_INTERRUPTION_DELIVERABLE_MESSAGE;

C_ASSERT(sizeof(HV_X64_INTERCEPT_MESSAGE_HEADER) == 0x28);
C_ASSERT(FIELD_OFFSET(HV_X64_MEMORY_INTERCEPT_MESSAGE, GuestPhysicalAddress) == 0x38);
C_ASSERT(FIELD_OFFSET(HV_X64_IO_PORT_INTERCEPT_MESSAGE, Rax) == 0x30);
C_ASSERT(FIELD_OFFSET(HV_X64_IO_PORT_INTERCEPT_MESSAGE, Rcx) == 0x68);
C_ASSERT(FIELD_OFFSET(HV_X64_CPUID_INTERCEPT_MESSAGE, DefaultResultRax) == 0x48);
C_ASSERT(FIELD_OFFSET(HV_X64_MSR_INTERCEPT_MESSAGE, Rax) == 0x38);

typedef struct _HV_MESSAGE
{
    ULONG MessageType;
    UCHAR PayloadSize;
    UCHAR MessageFlags;
    USHORT Reserved;
    ULONG64 OriginationId;
    UCHAR Payload[HV_MESSAGE_PAYLOAD_SIZE];
} HV_MESSAGE, *PHV_MESSAGE;

C_ASSERT(sizeof(HV_X64_INTERCEPT_MESSAGE_HEADER) == 40);
C_ASSERT(FIELD_OFFSET(HV_X64_MEMORY_INTERCEPT_MESSAGE, InstructionByteCount) == 44);
C_ASSERT(FIELD_OFFSET(HV_X64_MEMORY_INTERCEPT_MESSAGE, GuestPhysicalAddress) == 56);
C_ASSERT(FIELD_OFFSET(HV_X64_MEMORY_INTERCEPT_MESSAGE, InstructionBytes) == 64);
C_ASSERT(FIELD_OFFSET(HV_X64_IO_PORT_INTERCEPT_MESSAGE, InstructionByteCount) == 43);
C_ASSERT(FIELD_OFFSET(HV_X64_IO_PORT_INTERCEPT_MESSAGE, Rax) == 48);
C_ASSERT(FIELD_OFFSET(HV_X64_IO_PORT_INTERCEPT_MESSAGE, DsSegment) == 72);
C_ASSERT(FIELD_OFFSET(HV_X64_IO_PORT_INTERCEPT_MESSAGE, EsSegment) == 88);
C_ASSERT(FIELD_OFFSET(HV_X64_IO_PORT_INTERCEPT_MESSAGE, Rdi) == 120);
C_ASSERT(FIELD_OFFSET(HV_MESSAGE, Payload) == 16);

/*
 * Not in the TLFS: posted to the root when VPs of a partition may run again, one
 * bit per VP. Bank[] entries follow the set bits of BankMask in order.
 */
#define ReactVMessageSignalBitset                   0x80000100

typedef struct _REACTV_SIGNAL_BITSET
{
    ULONG64 PartitionId;
    ULONG64 Reserved[2];
    ULONG64 BankMask;
    ULONG64 Bank[8];
} REACTV_SIGNAL_BITSET, *PREACTV_SIGNAL_BITSET;

C_ASSERT(FIELD_OFFSET(REACTV_SIGNAL_BITSET, BankMask) == 24);
C_ASSERT(FIELD_OFFSET(REACTV_SIGNAL_BITSET, Bank) == 32);
C_ASSERT(sizeof(REACTV_SIGNAL_BITSET) <= HV_MESSAGE_PAYLOAD_SIZE);

C_ASSERT(sizeof(HV_REGISTER_ASSOC) == 32);
C_ASSERT(sizeof(HV_X64_SEGMENT_REGISTER) == 16);
C_ASSERT(sizeof(HV_X64_TABLE_REGISTER) == 16);
C_ASSERT(FIELD_OFFSET(HV_INPUT_MAP_GPA_PAGES, SourcePageList) == 0x18);
C_ASSERT(FIELD_OFFSET(HV_INPUT_CREATE_VP, SubnodeType) == 0x0F);
C_ASSERT(FIELD_OFFSET(HV_INPUT_CREATE_VP, Flags) == 0x20);
C_ASSERT(FIELD_OFFSET(HV_INPUT_SET_PARTITION_PROPERTY, PropertyValue) == 0x10);

/* 'Microsoft Hv' vendor signature */
#define REACTV_VENDOR_EBX                           0x7263694D
#define REACTV_VENDOR_ECX                           0x666F736F
#define REACTV_VENDOR_EDX                           0x76482074

/* Version reported in CPUID 0x40000002 */
#define REACTV_VERSION_BUILD                        1
#define REACTV_VERSION_MAJOR                        0
#define REACTV_VERSION_MINOR                        1
#define REACTV_VERSION_SERVICE_PACK                 0
#define REACTV_VERSION_SERVICE_NUMBER               0
#define REACTV_VERSION_SERVICE_BRANCH               0

#endif /* _REACTV_TLFS_H_ */
