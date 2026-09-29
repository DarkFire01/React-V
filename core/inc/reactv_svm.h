/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     AMD SVM architectural definitions (APM volume 2)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_SVM_H_
#define _REACTV_SVM_H_

/* CPUID leaves */
#define AMD64_CPUID_VENDOR              0x00000000
#define AMD64_CPUID_EXTENDED_MAX        0x80000000
#define AMD64_CPUID_EXTENDED_FEATURES   0x80000001
#define AMD64_CPUID_SVM_FEATURES        0x8000000A

/* Vendor strings as CPUID leaf 0 returns them in EBX, EDX, ECX */
#define AMD64_VENDOR_AMD_EBX            0x68747541  /* 'Auth' */
#define AMD64_VENDOR_AMD_EDX            0x69746E65  /* 'enti' */
#define AMD64_VENDOR_AMD_ECX            0x444D4163  /* 'cAMD' */
#define AMD64_VENDOR_INTEL_EBX          0x756E6547  /* 'Genu' */
#define AMD64_VENDOR_INTEL_EDX          0x49656E69  /* 'ineI' */
#define AMD64_VENDOR_INTEL_ECX          0x6C65746E  /* 'ntel' */

/* Fn8000_0001_ECX */
#define AMD64_EXT_FEATURE_SVM           0x00000004

/* Fn8000_000A_EAX holds the SVM revision in its low byte, EBX the ASID count */
#define AMD64_SVM_REVISION_MASK         0x000000FF

/* Fn8000_000A_EDX */
#define AMD64_SVM_FEATURE_NP            0x00000001
#define AMD64_SVM_FEATURE_LBR_VIRT      0x00000002
#define AMD64_SVM_FEATURE_SVM_LOCK      0x00000004
#define AMD64_SVM_FEATURE_NRIP_SAVE     0x00000008
#define AMD64_SVM_FEATURE_TSC_RATE_MSR  0x00000010
#define AMD64_SVM_FEATURE_VMCB_CLEAN    0x00000020
#define AMD64_SVM_FEATURE_FLUSH_BY_ASID 0x00000040
#define AMD64_SVM_FEATURE_DECODE_ASSIST 0x00000080
#define AMD64_SVM_FEATURE_PAUSE_FILTER  0x00000400
#define AMD64_SVM_FEATURE_AVIC          0x00002000

/* MSRs */
#define AMD64_MSR_PAT                   0x00000277
#define AMD64_MSR_EFER                  0xC0000080
#define AMD64_MSR_FS_BASE               0xC0000100
#define AMD64_MSR_GS_BASE               0xC0000101

/* x2APIC ICR and the INIT/INIT deassert/SIPI sequence; SIPI vector is the low byte */
#define REACTV_X2APIC_ICR               0x00000830
#define REACTV_ICR_INIT_ASSERT          0x00004500
#define REACTV_ICR_INIT_DEASSERT        0x00008500
#define REACTV_ICR_STARTUP              0x00004600

/* ICR field decode */
#define REACTV_ICR_MESSAGE_SHIFT        8
#define REACTV_ICR_MESSAGE_MASK         7
#define REACTV_ICR_MESSAGE_INIT         5
#define REACTV_ICR_MESSAGE_STARTUP      6
#define REACTV_ICR_VECTOR_MASK          0xFF
#define REACTV_ICR_DESTINATION_SHIFT    32
#define AMD64_MSR_KERNEL_GS_BASE        0xC0000102
#define AMD64_MSR_VM_CR                 0xC0010114
#define AMD64_MSR_VM_HSAVE_PA           0xC0010117

/* EFER */
#define AMD64_EFER_SVME                 0x0000000000001000ULL
#define AMD64_EFER_LMA                  0x0000000000000400ULL

/* CR0 */
#define AMD64_CR0_PE                    0x0000000000000001ULL
#define AMD64_CR0_AM                    0x0000000000040000ULL
#define AMD64_CR0_PG                    0x0000000080000000ULL

/* CR4 */
#define AMD64_CR4_PSE                   0x0000000000000010ULL
#define AMD64_CR4_PAE                   0x0000000000000020ULL

/* RFLAGS */
#define AMD64_RFLAGS_IF                 0x0000000000000200ULL

/*
 * Local APIC. Guests only get x2APIC mode: xAPIC is MMIO and would need
 * instruction decode to emulate.
 */
#define AMD64_MSR_APIC_BASE             0x0000001B
#define AMD64_APIC_BASE_BSP             0x0000000000000100ULL
#define AMD64_APIC_BASE_EXTENDED        0x0000000000000400ULL
#define AMD64_APIC_BASE_ENABLE          0x0000000000000800ULL
#define AMD64_APIC_BASE_ADDRESS_MASK    0x000FFFFFFFFFF000ULL
#define AMD64_APIC_DEFAULT_ADDRESS      0x00000000FEE00000ULL

/* x2APIC MSRs, 0x800 + (xAPIC offset >> 4) */
#define AMD64_X2APIC_FIRST              0x00000800
#define AMD64_X2APIC_LAST               0x000008FF
#define AMD64_X2APIC_ID                 0x00000802
#define AMD64_X2APIC_VERSION            0x00000803
#define AMD64_X2APIC_TPR                0x00000808
#define AMD64_X2APIC_PPR                0x0000080A
#define AMD64_X2APIC_EOI                0x0000080B
#define AMD64_X2APIC_LDR                0x0000080D
#define AMD64_X2APIC_SVR                0x0000080F
#define AMD64_X2APIC_ISR_FIRST          0x00000810
#define AMD64_X2APIC_TMR_FIRST          0x00000818
#define AMD64_X2APIC_IRR_FIRST          0x00000820
#define AMD64_X2APIC_ESR                0x00000828
#define AMD64_X2APIC_ICR                0x00000830
#define AMD64_X2APIC_LVT_TIMER          0x00000832
#define AMD64_X2APIC_LVT_ERROR          0x00000837
#define AMD64_X2APIC_TIMER_INITIAL      0x00000838
#define AMD64_X2APIC_TIMER_CURRENT      0x00000839
#define AMD64_X2APIC_TIMER_DIVIDE       0x0000083E
#define AMD64_X2APIC_SELF_IPI           0x0000083F

/* Emulated version and SVR reset values */
#define AMD64_APIC_VERSION_VALUE        0x00050014
#define AMD64_APIC_SVR_RESET            0x000000FF

/* ICR fields */
#define AMD64_APIC_ICR_VECTOR_MASK      0x00000000000000FFULL
#define AMD64_APIC_ICR_DELIVERY_SHIFT   8
#define AMD64_APIC_ICR_DELIVERY_MASK    0x07
#define AMD64_APIC_ICR_SHORTHAND_SHIFT  18
#define AMD64_APIC_ICR_SHORTHAND_MASK   0x03
#define AMD64_APIC_ICR_DESTINATION_SHIFT 32

/* ICR delivery modes */
#define AMD64_APIC_DELIVERY_FIXED       0
#define AMD64_APIC_DELIVERY_LOWEST      1
#define AMD64_APIC_DELIVERY_SMI         2
#define AMD64_APIC_DELIVERY_NMI         4
#define AMD64_APIC_DELIVERY_INIT        5
#define AMD64_APIC_DELIVERY_STARTUP     6

/* ICR destination shorthands */
#define AMD64_APIC_SHORTHAND_NONE       0
#define AMD64_APIC_SHORTHAND_SELF       1
#define AMD64_APIC_SHORTHAND_ALL        2
#define AMD64_APIC_SHORTHAND_OTHERS     3

#define AMD64_APIC_DESTINATION_ALL      0xFFFFFFFF

/* VM_CR */
#define AMD64_VM_CR_DPD                 0x00000001
#define AMD64_VM_CR_R_INIT              0x00000002
#define AMD64_VM_CR_DIS_A20M            0x00000004
#define AMD64_VM_CR_LOCK                0x00000008
#define AMD64_VM_CR_SVMDIS              0x00000010

#endif /* _REACTV_SVM_H_ */
