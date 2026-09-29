/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VMX instruction wrappers, launch and VM exit entry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <asm.inc>
#include "vmxasm.h"

EXTERN ReactVVmxHandleExit:PROC

.code64

/*
 * VMX instructions are emitted as bytes (as svm.S does for vmrun) so any
 * assembler works. CF set is VMfailInvalid, ZF set is VMfailValid.
 */

/*
 * BOOLEAN NTAPI ReactVVmxOn(ULONG64 Region<rcx>);
 *
 * vmxon takes a memory operand holding the physical address, so push it.
 */
PUBLIC ReactVVmxOn
ReactVVmxOn:
    push rcx
    /* vmxon qword ptr [rsp] */
    .byte HEX(F3), HEX(0F), HEX(C7), HEX(34), HEX(24)
    setbe al
    xor al, 1
    movzx eax, al
    pop rcx
    ret

/* VOID NTAPI ReactVVmxOff(VOID); */
PUBLIC ReactVVmxOff
ReactVVmxOff:
    /* vmxoff */
    .byte HEX(0F), HEX(01), HEX(C4)
    ret

/* BOOLEAN NTAPI ReactVVmxClear(ULONG64 Vmcs<rcx>); */
PUBLIC ReactVVmxClear
ReactVVmxClear:
    push rcx
    /* vmclear qword ptr [rsp] */
    .byte HEX(66), HEX(0F), HEX(C7), HEX(34), HEX(24)
    setbe al
    xor al, 1
    movzx eax, al
    pop rcx
    ret

/* BOOLEAN NTAPI ReactVVmxPtrLoad(ULONG64 Vmcs<rcx>); */
PUBLIC ReactVVmxPtrLoad
ReactVVmxPtrLoad:
    push rcx
    /* vmptrld qword ptr [rsp] */
    .byte HEX(0F), HEX(C7), HEX(34), HEX(24)
    setbe al
    xor al, 1
    movzx eax, al
    pop rcx
    ret

/* BOOLEAN NTAPI ReactVVmxWrite(ULONG Field<rcx>, ULONG64 Value<rdx>); */
PUBLIC ReactVVmxWrite
ReactVVmxWrite:
    /* vmwrite rcx, rdx */
    .byte HEX(0F), HEX(79), HEX(CA)
    setbe al
    xor al, 1
    movzx eax, al
    ret

/*
 * ULONG64 NTAPI ReactVVmxRead(ULONG Field<rcx>);
 *
 * Returns zero on failure, indistinguishable from a zero field.
 */
PUBLIC ReactVVmxRead
ReactVVmxRead:
    xor eax, eax
    /* vmread rax, rcx */
    .byte HEX(0F), HEX(78), HEX(C8)
    ret

/*
 * BOOLEAN NTAPI ReactVVmxEnterGuest(ULONG64 Vmcs<rcx>,
 *                                   ULONG64 HostCr3<rdx>,
 *                                   ULONG64 HostStackTop<r8>);
 *
 * Launches the root as a guest. On success vmlaunch does not return here: the
 * guest starts at ReactVVmxGuestResume on this frame. Falling through means
 * vmlaunch failed. Host CR3, RSP and RIP come from the VMCS on every exit.
 */
PUBLIC ReactVVmxEnterGuest
ReactVVmxEnterGuest:
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    /* Save loader CR3 and RSP at the top of the host stack */
    mov rax, cr3
    mov [r8 - 8], rax
    mov [r8 - 16], rsp

    /* Guest RSP is this frame */
    mov rcx, REACTV_VMCS_GUEST_RSP_ENC
    mov rdx, rsp
    /* vmwrite rcx, rdx */
    .byte HEX(0F), HEX(79), HEX(CA)
    jbe ReactVVmxLaunchRefused

    /* Host RSP starts below the saved values */
    mov rcx, REACTV_VMCS_HOST_RSP_ENC
    lea rdx, [r8 - 32]
    /* vmwrite rcx, rdx */
    .byte HEX(0F), HEX(79), HEX(CA)
    jbe ReactVVmxLaunchRefused

    /* vmlaunch */
    .byte HEX(0F), HEX(01), HEX(C2)

ReactVVmxLaunchRefused:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    xor eax, eax
    ret

/*
 * VOID NTAPI ReactVVmxGuestResume(VOID);
 *
 * Guest entry point after vmlaunch. Unwinds the EnterGuest frame and returns TRUE.
 */
PUBLIC ReactVVmxGuestResume
ReactVVmxGuestResume:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    mov eax, 1
    ret

/*
 * HOST_RIP. Only host RSP, CR3 and segments are loaded on exit, so the GPRs
 * still hold guest values and are saved first.
 */
PUBLIC ReactVVmxExitEntry
ReactVVmxExitEntry:
    /* 15 pushes follow, keep the call 16 byte aligned */
    sub rsp, 8

    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    /* XMM0-5 carry the XMM fast hypercall payload, save before any C code */
    sub rsp, 96
    movups [rsp], xmm0
    movups [rsp + 16], xmm1
    movups [rsp + 32], xmm2
    movups [rsp + 48], xmm3
    movups [rsp + 64], xmm4
    movups [rsp + 80], xmm5

    mov rcx, rsp
    sub rsp, 32
    call ReactVVmxHandleExit
    add rsp, 32

    test al, al
    jz ReactVVmxExitStop

    movups xmm0, [rsp]
    movups xmm1, [rsp + 16]
    movups xmm2, [rsp + 32]
    movups xmm3, [rsp + 48]
    movups xmm4, [rsp + 64]
    movups xmm5, [rsp + 80]
    add rsp, 96

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 8

    /* vmresume */
    .byte HEX(0F), HEX(01), HEX(C3)

/*
 * vmresume or the exit handler failed. No return path; halt for the debugger.
 */
ReactVVmxExitStop:
    cli
    hlt
    jmp ReactVVmxExitStop

END
