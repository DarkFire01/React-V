/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SVM guest entry, exit loop and child run stubs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <asm.inc>
#include "svmasm.h"

EXTERN ReactVSvmHandleExit:PROC

.code64

/*
 * BOOLEAN NTAPI ReactVSvmEnterGuest(ULONG64 Vmcb<rcx>,
 *                                   ULONG64 HostCr3<rdx>,
 *                                   ULONG64 HostStackTop<r8>);
 *
 * Launches the root as a guest and runs the #VMEXIT loop. Returns FALSE only
 * if vmrun fails. The root resumes on this frame via ReactVSvmGuestResume.
 *
 * #VMEXIT restores only RAX, RSP, RIP and control registers, so the loader's
 * CR3 and RSP are kept at the top of the host stack and all GPRs are saved
 * before any host code runs.
 */
/*
 * BOOLEAN NTAPI ReactVSvmEnterGuestAp(ULONG64 Vmcb<rcx>,
 *                                     ULONG64 HostCr3<rdx>,
 *                                     ULONG64 HostStackTop<r8>);
 *
 * Same, for an AP starting from SIPI state. Guest RSP is left as the VMCB has it.
 */
PUBLIC ReactVSvmEnterGuestAp
ReactVSvmEnterGuestAp:
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    jmp ReactVSvmEnterCommon

PUBLIC ReactVSvmEnterGuest
ReactVSvmEnterGuest:
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    /* Guest RSP for the root resume */
    mov [rcx + REACTV_VMCB_RSP_OFFSET], rsp

ReactVSvmEnterCommon:
    /* Save loader CR3, RSP and the VMCB at the host stack top */
    mov rax, cr3
    mov [r8 - 8], rax
    mov [r8 - 16], rsp
    mov [r8 - 24], rcx

    lea rsp, [r8 - 32]
    mov cr3, rdx
    mov rax, rcx

ReactVSvmRunLoop:
    /* vmrun rax (no ml64 mnemonic) */
    .byte HEX(0F), HEX(01), HEX(D8)

    /* Guest GPRs not in the VMCB, in the order ReactVSvmHandleExit reads them */
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

    /* Guest XMM0-5 (XMM fast hypercall payload), saved before any C code runs */
    sub rsp, 96
    movups [rsp], xmm0
    movups [rsp + 16], xmm1
    movups [rsp + 32], xmm2
    movups [rsp + 48], xmm3
    movups [rsp + 64], xmm4
    movups [rsp + 80], xmm5

    mov rbx, rax
    mov rcx, rax
    mov rdx, rsp
    sub rsp, 32
    call ReactVSvmHandleExit
    add rsp, 32

    test eax, eax
    jz ReactVSvmNeverRan

    /* Hypercall output returns in XMM0-5 */
    movups xmm0, [rsp]
    movups xmm1, [rsp + 16]
    movups xmm2, [rsp + 32]
    movups xmm3, [rsp + 48]
    movups xmm4, [rsp + 64]
    movups xmm5, [rsp + 80]
    add rsp, 96

    mov rax, rbx

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
    jmp ReactVSvmRunLoop

ReactVSvmNeverRan:
    add rsp, 208

    stgi

    mov rax, [rsp + 24]
    mov rcx, [rsp + 16]
    mov cr3, rax
    mov rsp, rcx

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
 * VOID NTAPI ReactVSvmRunChild(ULONG64 Vmcb<rcx>,
 *                              PREACTV_GUEST_REGISTERS Registers<rdx>,
 *                              ULONG64 HostVmcb<r8>);
 *
 * Runs a child VP for one vmrun and returns, for root scheduled VPs.
 *
 * Guest GPRs are pushed right after #VMEXIT in REACTV_GUEST_REGISTERS order and
 * copied to the caller's block. vmsave/vmload swap the state vmrun does not
 * handle (FS, GS, TR, LDTR, KernelGsBase, SYSCALL and SYSENTER MSRs) through
 * the host VMCB.
 */
PUBLIC ReactVSvmRunChild
ReactVSvmRunChild:
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    push rdx
    push rcx
    push r8

    /* Save host state to the host VMCB, load the guest's */
    mov rax, r8
    /* vmsave rax */
    .byte HEX(0F), HEX(01), HEX(DB)

    mov rax, rcx
    /* vmload rax */
    .byte HEX(0F), HEX(01), HEX(DA)

    mov r15, [rdx + 0]
    mov r14, [rdx + 8]
    mov r13, [rdx + 16]
    mov r12, [rdx + 24]
    mov r11, [rdx + 32]
    mov r10, [rdx + 40]
    mov r9, [rdx + 48]
    mov r8, [rdx + 56]
    mov rdi, [rdx + 64]
    mov rsi, [rdx + 72]
    mov rbp, [rdx + 80]
    mov rbx, [rdx + 88]
    mov rcx, [rdx + 104]
    /* RDX last, it holds the register block pointer */
    mov rdx, [rdx + 96]

    /* vmrun rax */
    .byte HEX(0F), HEX(01), HEX(D8)

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

    /* Save guest state to its VMCB, reload host state */
    /* vmsave rax */
    .byte HEX(0F), HEX(01), HEX(DB)
    mov rax, [rsp + 112]
    /* vmload rax */
    .byte HEX(0F), HEX(01), HEX(DA)

    /* Copy the 14 pushed GPRs to the caller's register block */
    mov rdi, [rsp + 128]
    mov rsi, rsp
    mov ecx, 14
    rep movsq

    add rsp, 136

    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret

/*
 * VOID NTAPI ReactVSvmGuestResume(VOID);
 *
 * Root guest RIP after launch. Not called; pops the ReactVSvmEnterGuest frame
 * and returns TRUE to its caller.
 */
PUBLIC ReactVSvmGuestResume
ReactVSvmGuestResume:
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
 * VOID NTAPI ReactVSvmAdoptDescriptors(PVOID Gdtr<rcx>, PVOID Idtr<rdx>,
 *                                      ULONG64 Code<r8>, ULONG64 Data<r9>);
 *
 * Loads the root's GDT, IDT, CS and data selectors on an AP, replacing the
 * startup page's minimal GDT. All four must change together: an IDT used with
 * stale selectors from another GDT triple faults, hence the far return for CS.
 */
PUBLIC ReactVSvmAdoptDescriptors
ReactVSvmAdoptDescriptors:
    lgdt fword ptr [rcx]

    /* Data and stack selectors first */
    mov ax, r9w
    mov ds, ax
    mov es, ax
    mov ss, ax

    /* Reload CS with a far return */
    push r8
    lea rax, ReactVSvmAdopted
    push rax
    /* retfq */
    .byte HEX(48), HEX(CB)

ReactVSvmAdopted:
    lidt fword ptr [rdx]
    ret

/*
 * VOID NTAPI ReactVSvmSaveFpu(PVOID Area<rcx>);
 * VOID NTAPI ReactVSvmRestoreFpu(PVOID Area<rcx>);
 *
 * fxsave/fxrstor of x87 and SSE state, which the VMCB does not switch. The area
 * is a page, satisfying the 16 byte alignment and 512 byte size.
 */
PUBLIC ReactVSvmSaveFpu
ReactVSvmSaveFpu:
    fxsave [rcx]
    ret

PUBLIC ReactVSvmRestoreFpu
ReactVSvmRestoreFpu:
    fxrstor [rcx]
    ret

END
