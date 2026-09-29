/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     freeldr entry point
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_FREELDR_H_
#define _REACTV_FREELDR_H_

/*
 * Launch the hypervisor under the loader if the boot options request it.
 * Call on the loader page tables, before the memory descriptor list goes to the kernel.
 */
VOID
NTAPI
ReactVFreeLdrLaunch(
    _In_ PCSTR BootOptions);

#endif /* _REACTV_FREELDR_H_ */
