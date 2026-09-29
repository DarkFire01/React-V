/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Segment selector reads with no compiler intrinsic
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <asm.inc>

.code64

// VOID NTAPI ReactVFldrReadSelectors(PUSHORT Selectors<rcx>);
PUBLIC ReactVFldrReadSelectors
ReactVFldrReadSelectors:
    mov ax, cs
    mov [rcx], ax
    mov ax, ss
    mov [rcx + 2], ax
    mov ax, ds
    mov [rcx + 4], ax
    mov ax, es
    mov [rcx + 6], ax
    mov ax, fs
    mov [rcx + 8], ax
    mov ax, gs
    mov [rcx + 10], ax
    str ax
    mov [rcx + 12], ax
    sldt ax
    mov [rcx + 14], ax
    ret

END
