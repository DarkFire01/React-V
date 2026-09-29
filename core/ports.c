/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Ports and connections
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"

/*
 * A port belongs to one partition and targets one of its VPs and SINTs. Other
 * partitions reach it only through a connection ID. VMBus is built on this.
 */

/* GLOBALS ********************************************************************/

static REACTV_PORT ReactVPort[REACTV_MAXIMUM_PORTS];
static REACTV_CONNECTION ReactVConnection[REACTV_MAXIMUM_CONNECTIONS];

/* FUNCTIONS ******************************************************************/

static
PREACTV_PORT
NTAPI
ReactVPortById(
    _In_ ULONG64 PartitionId,
    _In_ ULONG PortId)
{
    ULONG Index;

    for (Index = 0; Index < REACTV_MAXIMUM_PORTS; Index++)
    {
        PREACTV_PORT Port = &ReactVPort[Index];

        if (Port->Used && (Port->Id == PortId) &&
            (Port->OwnerPartitionId == PartitionId))
        {
            return Port;
        }
    }

    return NULL;
}

static
PREACTV_CONNECTION
NTAPI
ReactVConnectionById(
    _In_ ULONG ConnectionId)
{
    ULONG Index;

    for (Index = 0; Index < REACTV_MAXIMUM_CONNECTIONS; Index++)
    {
        if (ReactVConnection[Index].Used && (ReactVConnection[Index].Id == ConnectionId))
            return &ReactVConnection[Index];
    }

    return NULL;
}

/**
 * @brief
 * Delivers a message or event flag through a connection.
 *
 * @param[in] Message
 * Message to post, or NULL to signal an event.
 *
 * @param[in] Flag
 * Event flag number, relative to the port's base flag.
 *
 * @return
 * HV_STATUS_* code.
 */
static
ULONG64
NTAPI
ReactVPortDeliver(
    _In_ ULONG64 CallerId,
    _In_ ULONG ConnectionId,
    _In_opt_ PHV_MESSAGE Message,
    _In_ ULONG Flag,
    _Out_opt_ PBOOLEAN Newly)
{
    PREACTV_CONNECTION Connection = ReactVConnectionById(ConnectionId);
    BOOLEAN Signalled = FALSE;
    PREACTV_PARTITION Owner;
    PREACTV_PORT Port;
    ULONG64 Status;

    if (Newly != NULL)
        *Newly = FALSE;

    if (Connection == NULL)
        return HV_STATUS_INVALID_CONNECTION_ID;

    /* Only the partition the connection was made for may use it */
    if ((Connection->PartitionId != CallerId) &&
        (Connection->PartitionId != HV_PARTITION_ID_SELF))
    {
        return HV_STATUS_INVALID_CONNECTION_ID;
    }

    Port = &ReactVPort[Connection->PortIndex];
    if (!Port->Used)
        return HV_STATUS_INVALID_PORT_ID;

    Owner = ReactVPartitionFromId(Port->OwnerPartitionId);
    if (Owner == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    if (Message != NULL)
    {
        if (Port->Type != HvPortTypeMessage)
            return HV_STATUS_INVALID_PORT_ID;
    }
    else
    {
        if (Port->Type != HvPortTypeEvent)
            return HV_STATUS_INVALID_PORT_ID;

        /* Flag must be within the port's flag count */
        if ((Port->FlagCount != 0) && (Flag >= Port->FlagCount))
            return HV_STATUS_INVALID_PARAMETER;
    }

    /* Root ports go to the root SynIC, whose pages the core already has mapped */
    if (Owner == ReactVRootPartition())
    {
        if (Message != NULL)
        {
            Status = ReactVSynicPostToRoot(Port->TargetSint, Port->Id, Message);
        }
        else
        {
            Status = ReactVSynicSignalRoot(Port->TargetSint,
                                           Port->Id,
                                           Port->BaseFlagNumber + Flag,
                                           &Signalled);
        }
    }
    else
    {
        PREACTV_VP Vp;

        if (Port->TargetVp >= REACTV_MAXIMUM_VPS)
            return HV_STATUS_INVALID_VP_INDEX;

        Vp = &Owner->Vp[Port->TargetVp];
        if (!Vp->Created)
            return HV_STATUS_INVALID_VP_INDEX;

        if (Message != NULL)
        {
            Status = ReactVSynicPostMessage(Owner, Vp, Port->TargetSint, Message);
        }
        else
        {
            Status = ReactVSynicSignalEvent(Owner,
                                            Vp,
                                            Port->TargetSint,
                                            Port->BaseFlagNumber + Flag,
                                            &Signalled);
        }
    }

    if (Status != HV_STATUS_SUCCESS)
        return Status;

    /* Delivery count, read back through ReactVPortPropertyTaken */
    Port->Taken++;

    if (Newly != NULL)
        *Newly = Signalled;

    return HV_STATUS_SUCCESS;
}

/* HYPERCALLS *****************************************************************/

static
ULONG64
NTAPI
ReactVCreatePort(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_CREATE_PORT Input;
    PREACTV_PARTITION Owner;
    PREACTV_PORT Port = NULL;
    ULONG Index;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Owner = ReactVPartitionFromCall(Call, Input->PortPartitionId);
    if (Owner == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    if (ReactVPortById(Owner->Id, Input->PortId) != NULL)
        return HV_STATUS_INVALID_PORT_ID;

    for (Index = 0; Index < REACTV_MAXIMUM_PORTS; Index++)
    {
        if (!ReactVPort[Index].Used)
        {
            Port = &ReactVPort[Index];
            break;
        }
    }

    if (Port == NULL)
    {
        ReactVLogWarning("Port table full (%u)\n",
                         REACTV_MAXIMUM_PORTS);
        return HV_STATUS_NO_RESOURCES;
    }

    RtlZeroMemory(Port, sizeof(*Port));

    Port->Used = TRUE;
    Port->Id = Input->PortId;
    Port->Type = Input->PortType;
    Port->OwnerPartitionId = Owner->Id;
    Port->ConnectionPartitionId = Input->ConnectionPartitionId;
    Port->TargetSint = Input->TargetSint;
    Port->TargetVp = Input->TargetVp;
    Port->BaseFlagNumber = Input->BaseFlagNumber;
    Port->FlagCount = Input->FlagCount;
    Port->MonitorAddress = Input->MonitorAddress;

    ReactVLogTrace("Port %lx partition %I64u type %lu, VP %lu "
                   "SINT %lu, %u flags at %u\n",
                   Port->Id,
                   Port->OwnerPartitionId,
                   Port->Type,
                   Port->TargetVp,
                   Port->TargetSint,
                   Port->FlagCount,
                   Port->BaseFlagNumber);

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVConnectPort(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_CONNECT_PORT Input;
    PREACTV_PARTITION Caller;
    PREACTV_PARTITION Owner;
    PREACTV_CONNECTION Connection = NULL;
    PREACTV_PORT Port;
    ULONG Index;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Caller = ReactVPartitionFromCall(Call, Input->ConnectionPartitionId);
    Owner = ReactVPartitionFromCall(Call, Input->PortPartitionId);
    if ((Caller == NULL) || (Owner == NULL))
        return HV_STATUS_INVALID_PARTITION_ID;

    Port = ReactVPortById(Owner->Id, Input->PortId);
    if (Port == NULL)
        return HV_STATUS_INVALID_PORT_ID;

    if (ReactVConnectionById(Input->ConnectionId) != NULL)
        return HV_STATUS_INVALID_CONNECTION_ID;

    for (Index = 0; Index < REACTV_MAXIMUM_CONNECTIONS; Index++)
    {
        if (!ReactVConnection[Index].Used)
        {
            Connection = &ReactVConnection[Index];
            break;
        }
    }

    if (Connection == NULL)
    {
        ReactVLogWarning("Connection table full (%u)\n",
                         REACTV_MAXIMUM_CONNECTIONS);
        return HV_STATUS_NO_RESOURCES;
    }

    Connection->Used = TRUE;
    Connection->Id = Input->ConnectionId;
    Connection->PartitionId = Caller->Id;
    Connection->PortIndex = (ULONG)(Port - &ReactVPort[0]);

    ReactVLogTrace("Connection %lx partition %I64u to port %lx\n",
                   Connection->Id,
                   Connection->PartitionId,
                   Port->Id);

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVDeletePort(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_NAME_PORT Input;
    PREACTV_PARTITION Owner;
    PREACTV_PORT Port;
    ULONG Index;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Owner = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Owner == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    Port = ReactVPortById(Owner->Id, Input->Id);
    if (Port == NULL)
        return HV_STATUS_INVALID_PORT_ID;

    /* Drop every connection to this port */
    for (Index = 0; Index < REACTV_MAXIMUM_CONNECTIONS; Index++)
    {
        if (ReactVConnection[Index].Used &&
            (&ReactVPort[ReactVConnection[Index].PortIndex] == Port))
        {
            RtlZeroMemory(&ReactVConnection[Index], sizeof(ReactVConnection[Index]));
        }
    }

    RtlZeroMemory(Port, sizeof(*Port));

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVDisconnectPort(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_NAME_PORT Input;
    PREACTV_PARTITION Caller;
    PREACTV_CONNECTION Connection;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Caller = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Caller == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    Connection = ReactVConnectionById(Input->Id);
    if (Connection == NULL)
        return HV_STATUS_INVALID_CONNECTION_ID;

    RtlZeroMemory(Connection, sizeof(*Connection));

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Returns a port property.
 *
 * @remarks
 * Only ReactVPortPropertyTaken is supported. The root reads it before deleting
 * a port and compares it against its own delivery count.
 */
static
ULONG64
NTAPI
ReactVGetPortProperty(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_GET_PORT_PROPERTY Input;
    PREACTV_PARTITION Owner;
    PREACTV_PORT Port;
    PULONG64 Output;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    Output = ReactVHypercallOutput(Call, sizeof(*Input), sizeof(*Output));
    if ((Input == NULL) || (Output == NULL))
        return HV_STATUS_INVALID_PARAMETER;

    Owner = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Owner == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    Port = ReactVPortById(Owner->Id, Input->PortId);
    if (Port == NULL)
        return HV_STATUS_INVALID_PORT_ID;

    if (Input->PropertyCode != ReactVPortPropertyTaken)
    {
        ReactVLogWarning("Port %lx unsupported property %lu, returning 0\n",
                         Port->Id,
                         Input->PropertyCode);
        *Output = 0;
        return HV_STATUS_SUCCESS;
    }

    *Output = Port->Taken;

    return HV_STATUS_SUCCESS;
}

/**
 * @brief
 * Stores a port property.
 *
 * @remarks
 * The root sets these on every event port and never reads them back.
 */
static
ULONG64
NTAPI
ReactVSetPortProperty(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_SET_PORT_PROPERTY Input;
    PREACTV_PARTITION Owner;
    PREACTV_PORT Port;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Owner = ReactVPartitionFromCall(Call, Input->PartitionId);
    if (Owner == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    Port = ReactVPortById(Owner->Id, Input->PortId);
    if (Port == NULL)
        return HV_STATUS_INVALID_PORT_ID;

    if (Input->PropertyCode >= REACTV_PORT_PROPERTIES)
    {
        ReactVLogWarning("Unsupported port property %I64u\n",
                         Input->PropertyCode);
        return HV_STATUS_INVALID_PARAMETER;
    }

    Port->Property[Input->PropertyCode] = Input->PropertyValue;

    return HV_STATUS_SUCCESS;
}

static
ULONG64
NTAPI
ReactVPostMessage(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_POST_MESSAGE Input;
    PREACTV_PARTITION Caller;
    HV_MESSAGE Message;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    if (Input->PayloadSize > HV_MESSAGE_PAYLOAD_SIZE)
        return HV_STATUS_INVALID_PARAMETER;

    Caller = ReactVCallerPartition(Call);
    if (Caller == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    RtlZeroMemory(&Message, sizeof(Message));

    Message.MessageType = Input->MessageType;
    Message.PayloadSize = (UCHAR)Input->PayloadSize;

    RtlCopyMemory(Message.Payload, Input->Payload, Input->PayloadSize);

    return ReactVPortDeliver(Caller->Id, Input->ConnectionId, &Message, 0, NULL);
}

static
ULONG64
NTAPI
ReactVSignalEvent(
    _In_ PREACTV_HYPERCALL Call)
{
    PHV_INPUT_SIGNAL_EVENT Input;
    PREACTV_PARTITION Caller;
    BOOLEAN Newly = FALSE;

    Input = ReactVHypercallInput(Call, sizeof(*Input));
    if (Input == NULL)
        return HV_STATUS_INVALID_PARAMETER;

    Caller = ReactVCallerPartition(Call);
    if (Caller == NULL)
        return HV_STATUS_INVALID_PARTITION_ID;

    return ReactVPortDeliver(Caller->Id,
                             Input->ConnectionId,
                             NULL,
                             Input->FlagNumber,
                             &Newly);
}

/**
 * @brief
 * Frees the ports a partition owns and every connection from it or to its ports.
 */
VOID
NTAPI
ReactVReleasePorts(
    _In_ ULONG64 PartitionId)
{
    ULONG Index;

    for (Index = 0; Index < REACTV_MAXIMUM_CONNECTIONS; Index++)
    {
        PREACTV_CONNECTION Connection = &ReactVConnection[Index];

        if (!Connection->Used)
            continue;

        if ((Connection->PartitionId == PartitionId) ||
            (ReactVPort[Connection->PortIndex].OwnerPartitionId == PartitionId))
        {
            RtlZeroMemory(Connection, sizeof(*Connection));
        }
    }

    for (Index = 0; Index < REACTV_MAXIMUM_PORTS; Index++)
    {
        PREACTV_PORT Port = &ReactVPort[Index];

        if (Port->Used && (Port->OwnerPartitionId == PartitionId))
            RtlZeroMemory(Port, sizeof(*Port));
    }
}

_Must_inspect_result_
BOOLEAN
NTAPI
ReactVPortHypercall(
    _In_ PREACTV_HYPERCALL Call,
    _Out_ ULONG64 *Status,
    _Out_ ULONG64 *RepsCompleted)
{
    *RepsCompleted = 0;

    switch (Call->Code)
    {
        case HvCallCreatePort:
            *Status = ReactVCreatePort(Call);
            return TRUE;

        case HvCallConnectPort:
            *Status = ReactVConnectPort(Call);
            return TRUE;

        case HvCallDeletePort:
            *Status = ReactVDeletePort(Call);
            return TRUE;

        case HvCallDisconnectPort:
            *Status = ReactVDisconnectPort(Call);
            return TRUE;

        case HvCallSetPortProperty:
            *Status = ReactVSetPortProperty(Call);
            return TRUE;

        case HvCallPostMessage:
            *Status = ReactVPostMessage(Call);
            return TRUE;

        case HvCallSignalEvent:
            *Status = ReactVSignalEvent(Call);
            return TRUE;

        case HvCallGetPortProperty:
            *Status = ReactVGetPortProperty(Call);
            return TRUE;

        case HvCallNotifyPortRingEmpty:
            /* The core never queues overflow, so there is nothing to refill */
            *Status = HV_STATUS_SUCCESS;
            return TRUE;

        default:
            break;
    }

    return FALSE;
}

/* EOF */
