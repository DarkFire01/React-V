/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     winload shim internal header
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_SHIM_H_
#define _REACTV_SHIM_H_

#include <ntddk.h>

#include "reactv_loader.h"
#include "reactv_svm.h"

extern REACTV_MEMORY_IFACE ReactVShimMemory;
extern REACTV_SYSTEM_IFACE ReactVShimSystem;
extern REACTV_LAUNCH_IFACE ReactVShimLaunch;
extern REACTV_DEBUG_IFACE ReactVShimDebug;

NTSTATUS
NTAPI
ReactVWinLoadLaunch(VOID);

#endif /* _REACTV_SHIM_H_ */
