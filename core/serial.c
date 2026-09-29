/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Core serial log
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "reactv_core.h"

#include <stdio.h>

/*
 * The shim log lives in freeldr.sys, outside the core address space, so the
 * core drives COM1 itself. It shares the port with the kernel debugger so both
 * logs interleave in order. The root never runs while the core writes, so lines
 * may split but bytes are never lost.
 */
#define REACTV_CORE_PORT 0x3F8 /* COM1 */

#define REACTV_UART_DATA        0
#define REACTV_UART_INTERRUPT   1
#define REACTV_UART_FIFO        2
#define REACTV_UART_LINE        3
#define REACTV_UART_MODEM       4
#define REACTV_UART_STATUS      5
#define REACTV_UART_SCRATCH     7

#define REACTV_UART_DLAB        0x80
#define REACTV_UART_8N1         0x03
#define REACTV_UART_FIFO_ON     0x07
#define REACTV_UART_DTR_RTS     0x03
#define REACTV_UART_HOLDING_EMPTY 0x20

#define REACTV_LOG_BUFFER 256

/* GLOBALS ********************************************************************/

static USHORT ReactVCorePort;

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
ReactVSerialWrite(
    _In_z_ PCSTR Text);

static
BOOLEAN
NTAPI
ReactVSerialPresent(
    _In_ USHORT Port)
{
    /* 16550 scratch register loopback */
    __outbyte(Port + REACTV_UART_SCRATCH, 0xAA);
    if (__inbyte(Port + REACTV_UART_SCRATCH) != 0xAA)
        return FALSE;

    __outbyte(Port + REACTV_UART_SCRATCH, 0x55);
    return (__inbyte(Port + REACTV_UART_SCRATCH) == 0x55);
}

VOID
NTAPI
ReactVSerialInitialize(VOID)
{
    /* Called twice per launch */
    if (ReactVCorePort != 0)
        return;

    /*
     * Take the port even if the scratch test fails: older UARTs have no scratch
     * register, and writing to an absent port is harmless.
     */
    ReactVCorePort = REACTV_CORE_PORT;

    __outbyte(ReactVCorePort + REACTV_UART_INTERRUPT, 0);
    __outbyte(ReactVCorePort + REACTV_UART_LINE, REACTV_UART_DLAB);
    /* Divisor 1, 115200 baud */
    __outbyte(ReactVCorePort + REACTV_UART_DATA, 1);
    __outbyte(ReactVCorePort + REACTV_UART_INTERRUPT, 0);
    __outbyte(ReactVCorePort + REACTV_UART_LINE, REACTV_UART_8N1);
    __outbyte(ReactVCorePort + REACTV_UART_FIFO, REACTV_UART_FIFO_ON);
    __outbyte(ReactVCorePort + REACTV_UART_MODEM, REACTV_UART_DTR_RTS);

    /* Unconditional first line, proves the core ran */
    ReactVSerialWrite("\r\nreactv: core entry\r\n");

    if (!ReactVSerialPresent(REACTV_CORE_PORT))
        ReactVSerialWrite("reactv: UART scratch test failed, using COM1 anyway\r\n");
}

/* Lets the shim send its log here once the core owns the port */
BOOLEAN
NTAPI
ReactVSerialReady(VOID)
{
    return (ReactVCorePort != 0);
}

static
VOID
NTAPI
ReactVSerialWrite(
    _In_z_ PCSTR Text)
{
    ULONG Index;

    if (ReactVCorePort == 0)
        return;

    for (Index = 0; Text[Index] != ANSI_NULL; Index++)
    {
        ULONG Spin = 0;

        /* Bounded spin so a dead port cannot hang the machine */
        while (!(__inbyte(ReactVCorePort + REACTV_UART_STATUS) & REACTV_UART_HOLDING_EMPTY))
        {
            if (++Spin > 0x100000)
                return;
        }

        if (Text[Index] == '\n')
            __outbyte(ReactVCorePort + REACTV_UART_DATA, '\r');

        __outbyte(ReactVCorePort + REACTV_UART_DATA, Text[Index]);
    }
}

/* Raw write to the core port, for callers outside the core */
VOID
NTAPI
ReactVSerialPut(
    _In_z_ PCSTR Text)
{
    ReactVSerialWrite(Text);
}

VOID
__cdecl
ReactVLog(
    _In_ ULONG Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    CHAR Buffer[REACTV_LOG_BUFFER];
    va_list Arguments;

    va_start(Arguments, Format);
    _vsnprintf(Buffer, sizeof(Buffer), Format, Arguments);
    va_end(Arguments);
    Buffer[sizeof(Buffer) - 1] = ANSI_NULL;

    /* No port: fall back to the loader log, only before launch and once the interface is set */
    if (ReactVCorePort == 0)
    {
        if (!ReactVCore.Live &&
            (ReactVCore.Loader != NULL) &&
            (ReactVCore.Loader->Debug != NULL))
        {
            ReactVCore.Loader->Debug->Log(Level, "%s", Buffer);
        }

        return;
    }

    ReactVSerialWrite("reactv: ");
    ReactVSerialWrite(Buffer);
}
