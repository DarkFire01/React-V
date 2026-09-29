/*
 * PROJECT:     ReactV
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VMCB layout (AMD APM vol. 2, appendix B)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _REACTV_VMCB_H_
#define _REACTV_VMCB_H_

/* Intercepts at control offset 0x00C */
#define REACTV_INTERCEPT_INTR       0x00000001
#define REACTV_INTERCEPT_NMI        0x00000002
#define REACTV_INTERCEPT_SMI        0x00000004
#define REACTV_INTERCEPT_VINTR      0x00000010
#define REACTV_INTERCEPT_INIT       0x00000008
#define REACTV_INTERCEPT_CPUID      0x00040000
#define REACTV_INTERCEPT_HLT        0x01000000
#define REACTV_INTERCEPT_IOIO       0x08000000
#define REACTV_INTERCEPT_MSR        0x10000000
#define REACTV_INTERCEPT_SHUTDOWN   0x80000000

/* Intercepts at control offset 0x010 */
#define REACTV_INTERCEPT_VMRUN      0x00000001
#define REACTV_INTERCEPT_VMMCALL    0x00000002

/* TLB_CONTROL */
#define REACTV_TLB_NOTHING          0
#define REACTV_TLB_FLUSH_ALL        1
#define REACTV_TLB_FLUSH_GUEST      3
#define REACTV_TLB_FLUSH_GUEST_LOCAL 7

/* NP_ENABLE */
#define REACTV_NESTED_PAGING_ENABLE 0x0000000000000001ULL

/* Handled EXITCODE values */
#define REACTV_EXIT_INTR            0x0000000000000060ULL
#define REACTV_EXIT_NMI             0x0000000000000061ULL
#define REACTV_EXIT_VINTR           0x0000000000000064ULL
#define REACTV_EXIT_CPUID           0x0000000000000072ULL
#define REACTV_EXIT_HLT             0x0000000000000078ULL
#define REACTV_EXIT_IOIO            0x000000000000007BULL
#define REACTV_EXIT_MSR             0x000000000000007CULL
#define REACTV_EXIT_SHUTDOWN        0x000000000000007FULL
#define REACTV_EXIT_VMRUN           0x0000000000000080ULL
#define REACTV_EXIT_VMMCALL         0x0000000000000081ULL
#define REACTV_EXIT_NESTED_FAULT    0x0000000000000400ULL
#define REACTV_EXIT_INVALID         0xFFFFFFFFFFFFFFFFULL

/* EXITINFO1 of an IOIO intercept */
#define REACTV_IOIO_TYPE_IN         0x00000001
#define REACTV_IOIO_STRING          0x00000004
#define REACTV_IOIO_REPEAT          0x00000008
#define REACTV_IOIO_SIZE_MASK       0x00000070
#define REACTV_IOIO_SIZE_SHIFT      4
#define REACTV_IOIO_PORT_SHIFT      16

/* IOIO access size, after masking and shifting */
#define REACTV_IOIO_SIZE_8          1
#define REACTV_IOIO_SIZE_16         2
#define REACTV_IOIO_SIZE_32         4

/* EXITINFO1 of a nested page fault, #PF error code format */
#define REACTV_NESTED_FAULT_PRESENT 0x0000000000000001ULL
#define REACTV_NESTED_FAULT_WRITE   0x0000000000000002ULL
#define REACTV_NESTED_FAULT_USER    0x0000000000000004ULL
#define REACTV_NESTED_FAULT_FETCH   0x0000000000000010ULL

/* Memory type reported to the guest */
#define REACTV_CACHE_WRITE_BACK     6

/* EVENTINJ */
#define REACTV_EVENT_TYPE_INTERRUPT 0
#define REACTV_EVENT_TYPE_NMI       2
#define REACTV_EVENT_TYPE_EXCEPTION 3
#define REACTV_EVENT_TYPE_SOFTWARE  4
#define REACTV_EVENT_TYPE_SHIFT     8
#define REACTV_EVENT_TYPE_MASK      0x00000007
#define REACTV_EVENT_VECTOR_MASK    0x000000FF
#define REACTV_EVENT_ERROR_VALID    0x00000800
#define REACTV_EVENT_VALID          0x80000000
#define REACTV_EVENT_ERROR_SHIFT    32
#define REACTV_VECTOR_GP_FAULT      13

/*
 * V_INTR at control offset 0x060. A pending V_IRQ with VINTR intercepted is used
 * as an interrupt window exit.
 */
#define REACTV_VINTR_IRQ            0x0000000000000100ULL
#define REACTV_VINTR_PRIORITY_SHIFT 16
#define REACTV_VINTR_IGNORE_TPR     0x0000000000100000ULL
#define REACTV_VINTR_MASKING        0x0000000001000000ULL
#define REACTV_VINTR_VECTOR_SHIFT   32

/* INTERRUPT_SHADOW at control offset 0x068 */
#define REACTV_SHADOW_INTERRUPT     0x0000000000000001ULL
#define REACTV_SHADOW_GUEST_MASK    0x0000000000000002ULL

/* FXSAVE area */
typedef struct _REACTV_FPU_STATE
{
    USHORT ControlWord;  
    USHORT StatusWord;   
    UCHAR TagWord;       
    UCHAR Reserved1;
    USHORT Opcode;                     
    ULONG64 InstructionPointer; 
    ULONG64 DataPointer;        
    ULONG MxCsr;                
    ULONG MxCsrMask;            
    UCHAR Stack[8][16];         
    UCHAR Vector[16][16];       
    UCHAR Reserved2[96];        
} REACTV_FPU_STATE, *PREACTV_FPU_STATE;

C_ASSERT(sizeof(REACTV_FPU_STATE) == 512);
C_ASSERT(FIELD_OFFSET(REACTV_FPU_STATE, Vector) == 0x0A0);

/* FPU/SSE reset values, a zeroed area does not match them */
#define REACTV_FPU_RESET_CONTROL    0x037F
#define REACTV_FPU_RESET_MXCSR      0x1F80
#define REACTV_FPU_MXCSR_MASK       0x0000FFBF

/* DR6/DR7 reset values, the loader never sets them */
#define REACTV_DR6_RESET            0x00000000FFFF0FF0ULL
#define REACTV_DR7_RESET            0x0000000000000400ULL

typedef struct _REACTV_VMCB_SEGMENT
{
    USHORT Selector;
    USHORT Attributes;
    ULONG Limit;
    ULONG64 Base;
} REACTV_VMCB_SEGMENT, *PREACTV_VMCB_SEGMENT;

C_ASSERT(sizeof(REACTV_VMCB_SEGMENT) == 16);

typedef struct _REACTV_VMCB_CONTROL
{
    USHORT InterceptCrRead;               
    USHORT InterceptCrWrite;        
    USHORT InterceptDrRead;         
    USHORT InterceptDrWrite;        
    ULONG InterceptException;       
    ULONG Intercept1;               
    ULONG Intercept2;               
    ULONG Intercept3;               
    UCHAR Reserved1[0x03C - 0x018];
    USHORT PauseFilterThreshold; 
    USHORT PauseFilterCount;     
    ULONG64 IopmBasePa;          
    ULONG64 MsrpmBasePa;         
    ULONG64 TscOffset;           
    ULONG GuestAsid;             
    UCHAR TlbControl;        
    UCHAR Reserved2[3];
    ULONG64 VirtualInterrupt; 
    ULONG64 InterruptShadow;  
    ULONG64 ExitCode;         
    ULONG64 ExitInfo1;        
    ULONG64 ExitInfo2;        
    ULONG64 ExitIntInfo;      
    ULONG64 NestedPaging;     
    ULONG64 AvicApicBar;      
    ULONG64 GhcbPhysical;     
    ULONG64 EventInject;      
    ULONG64 NestedCr3;        
    ULONG64 VirtualExtensions;
    ULONG VmcbClean;          
    ULONG Reserved3;
    ULONG64 NextRip;               
    UCHAR FetchedInstructionLength;
    UCHAR FetchedInstruction[15];  
    UCHAR Reserved4[0x400 - 0x0E0];
} REACTV_VMCB_CONTROL, *PREACTV_VMCB_CONTROL;

C_ASSERT(sizeof(REACTV_VMCB_CONTROL) == 0x400);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, Intercept2) == 0x010);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, PauseFilterThreshold) == 0x03C);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, IopmBasePa) == 0x040);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, GuestAsid) == 0x058);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, TlbControl) == 0x05C);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, ExitCode) == 0x070);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, NestedPaging) == 0x090);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, EventInject) == 0x0A8);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, NestedCr3) == 0x0B0);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, VmcbClean) == 0x0C0);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_CONTROL, NextRip) == 0x0C8);

typedef struct _REACTV_VMCB_SAVE
{
    REACTV_VMCB_SEGMENT Es;  
    REACTV_VMCB_SEGMENT Cs;  
    REACTV_VMCB_SEGMENT Ss;  
    REACTV_VMCB_SEGMENT Ds;  
    REACTV_VMCB_SEGMENT Fs;  
    REACTV_VMCB_SEGMENT Gs;  
    REACTV_VMCB_SEGMENT Gdtr;
    REACTV_VMCB_SEGMENT Ldtr;
    REACTV_VMCB_SEGMENT Idtr;
    REACTV_VMCB_SEGMENT Tr;  
    UCHAR Reserved1[0x0CB - 0x0A0];
    UCHAR Cpl;
    ULONG Reserved2;
    ULONG64 Efer;
    UCHAR Reserved3[0x148 - 0x0D8];
    ULONG64 Cr4;   
    ULONG64 Cr3;   
    ULONG64 Cr0;   
    ULONG64 Dr7;   
    ULONG64 Dr6;   
    ULONG64 Rflags;
    ULONG64 Rip;   
    UCHAR Reserved4[0x1D8 - 0x180];
    ULONG64 Rsp;         
    ULONG64 SCet;        
    ULONG64 Ssp;         
    ULONG64 IsstAddr;    
    ULONG64 Rax;         
    ULONG64 Star;        
    ULONG64 LStar;       
    ULONG64 CStar;       
    ULONG64 SfMask;      
    ULONG64 KernelGsBase;
    ULONG64 SysenterCs;  
    ULONG64 SysenterEsp; 
    ULONG64 SysenterEip; 
    ULONG64 Cr2;         
    UCHAR Reserved5[0x268 - 0x248];
    ULONG64 GuestPat;
    ULONG64 DebugControl;
    ULONG64 BranchFrom;
    ULONG64 BranchTo;
    ULONG64 LastExceptionFrom;
    ULONG64 LastExceptionTo;
} REACTV_VMCB_SAVE, *PREACTV_VMCB_SAVE;

C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Cs) == 0x010);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Gdtr) == 0x060);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Tr) == 0x090);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Cpl) == 0x0CB);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Efer) == 0x0D0);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Cr4) == 0x148);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Cr0) == 0x158);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Rflags) == 0x170);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Rip) == 0x178);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Rsp) == 0x1D8);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Rax) == 0x1F8);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, KernelGsBase) == 0x220);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, Cr2) == 0x240);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB_SAVE, GuestPat) == 0x268);

typedef struct _REACTV_VMCB
{
    REACTV_VMCB_CONTROL Control;
    REACTV_VMCB_SAVE Save;
    UCHAR Reserved[AMD64_PAGE_SIZE - 0x400 - sizeof(REACTV_VMCB_SAVE)];
} REACTV_VMCB, *PREACTV_VMCB;

C_ASSERT(FIELD_OFFSET(REACTV_VMCB, Save) == 0x400);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB, Save.Rip) == 0x578);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB, Save.Rsp) == 0x5D8);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB, Save.Rax) == 0x5F8);
C_ASSERT(FIELD_OFFSET(REACTV_VMCB, Save.GuestPat) == 0x668);
C_ASSERT(sizeof(REACTV_VMCB) == AMD64_PAGE_SIZE);

#endif /* _REACTV_VMCB_H_ */
