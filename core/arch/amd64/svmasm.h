/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VMCB offsets used by the SVM launch stub
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_SVMASM_H_
#define _REACTV_SVMASM_H_

/*
 * The stub writes the root's resume RSP into the VMCB. Decimal so both assemblers
 * and the compiler parse it; svm.c asserts it matches the structure.
 */
#define REACTV_VMCB_RSP_OFFSET 1496 /* 0x5D8 */

#endif /* _REACTV_SVMASM_H_ */
