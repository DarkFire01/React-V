/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VMCS field encodings used by the VMX launch stub
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_VMXASM_H_
#define _REACTV_VMXASM_H_

/*
 * Guest and host RSP, written by the stub. Decimal so both assemblers and the
 * compiler parse them; vmx.c asserts they match the field encodings.
 */
#define REACTV_VMCS_GUEST_RSP_ENC 26652 /* 0x681C */
#define REACTV_VMCS_HOST_RSP_ENC  27668 /* 0x6C14 */

#endif /* _REACTV_VMXASM_H_ */
