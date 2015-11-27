
/***************************************************************************
*
*   Copyright (c) 2008 Jeff V. Merkey  All Rights Reserved.
*   jeffmerkey@gmail.com
*
*   This program is free software; you can redistribute it and/or modify it
*   under the terms of the GNU General Public License as published by the
*   Free Software Foundation, version 2.
*
*   This program is distributed in the hope that it will be useful, but
*   WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*   General Public License for more details.
*
*   You are free to modify and re-distribute this program in accordance
*   with the terms specified in the GNU Public License.  The copyright
*   contained in this code is required to be present in any derivative
*   works and you are required to provide the source code for this
*   program as part of any commercial or non-commercial distribution.
*   You are required to respect the rights of the Copyright holders
*   named within this code.
*
*   jeffmerkey@gmail.com is the official maintainer of
*   this code.  You are encouraged to report any bugs, problems, fixes,
*   suggestions, and comments about this software.
*
*   AUTHOR   :  Jeff V. Merkey
*   DESCRIP  :  Merkey's Linux Debugger
*
***************************************************************************/

#include <linux/version.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/mm.h>
#include <linux/cdrom.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/ctype.h>
#include <linux/keyboard.h>
#include <linux/console.h>
#include <linux/serial_reg.h>
#include <linux/uaccess.h>
#include <linux/nmi.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/atomic.h>
#include <asm/msr.h>
#include <asm/ptrace.h>
#include <linux/io.h>
#include <linux/clocksource.h>

#if defined(CONFIG_SMP)
#include <asm/apic.h>
#include <asm/ipi.h>
#include <linux/cpumask.h>
#endif

#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>
#include <linux/kallsyms.h>

#include "mdb.h"
#include "mdb-ia.h"
#include "mdb-list.h"
#include "mdb-ia-proc.h"
#include "mdb-base.h"
#include "mdb-proc.h"
#include "mdb-os.h"
#include "mdb-keyboard.h"

unsigned long MajorVersion = 2627;
unsigned long MinorVersion = 9042008;
unsigned long BuildVersion = 6;

unsigned char *IA32Flags[]=
{
   "CF", 0, "PF", 0, "AF",    0, "ZF", "SF", "TF", "IF", "DF", "OF",
   0,    0, "NT", 0, "RF", "VM", "AC", "VIF","VIP","ID",    0,    0,
   0,
};

unsigned char *BreakDescription[]=
{
   "EXECUTE",  "WRITE",  "IOPORT",  "READ/WRITE",
};

unsigned char *BreakLengthDescription[]={
   ": 1 BYTE",  ": 2 BYTE",  ": ??????",  ": 4 BYTE",
};

unsigned char *ExceptionDescription[]={
   "Divide By Zero",                 /*  0 */
   "Debugger Exception (INT1)",      /*  1 */
   "Non-Maskable Interrupt",         /*  2 */
   "Debugger Breakpoint (INT3)",     /*  3 */
   "Overflow Exception",             /*  4 */
   "Bounds Check",                   /*  5 */
   "Invalid Opcode",                 /*  6 */
   "No Coprocessor",                 /*  7 */
   "Double Fault",                   /*  8 */
   "Cops Error",                     /*  9 */
   "Invalid Task State Segment",     /*  10 */
   "Segment Not Present",            /*  11 */
   "Stack Exception",                /*  12 */
   "General Protection",             /*  13 */
   "Page Fault",                     /*  14 */
   "InvalidInterrupt",               /*  15 */
   "Coprocessor Error",              /*  16 */
   "AlignmentCheck",                 /*  17 */
   "Machine Check",                  /*  18 */
   "Enter Debugger Request",         /*  19 */
   "Unvectored Exception",           /*  20 */
   "Directed NMI Breakpoint",        /*  21 */
   "Panic"                           /*  22 */
};
unsigned long exceptions = (sizeof(ExceptionDescription) / sizeof(unsigned char *));

unsigned char char32spc[] = { "xxxúxxxúxxxúxxxùxxxúxxxúxxxúxxx " };
unsigned char flset[] = { "VMRF  NT    OFDNIETFMIZR  AC  PE  CY" };
unsigned char floff[] = { "              UPID  PLNZ      PO  NC" };
unsigned char fluse[] = { 1,1,0,1,0,0,1,1,1,1,1,1,0,1,0,1,0,1 };

unsigned long MTRR_BASE_REGS[] = {
  MTRR_PHYS_BASE_0, MTRR_PHYS_BASE_1, MTRR_PHYS_BASE_2, MTRR_PHYS_BASE_3,
  MTRR_PHYS_BASE_4, MTRR_PHYS_BASE_5, MTRR_PHYS_BASE_6, MTRR_PHYS_BASE_7
};

unsigned long MTRR_MASK_VALUES[] = {
  MTRR_PHYS_MASK_0, MTRR_PHYS_MASK_1, MTRR_PHYS_MASK_2, MTRR_PHYS_MASK_3,
  MTRR_PHYS_MASK_4, MTRR_PHYS_MASK_5, MTRR_PHYS_MASK_6, MTRR_PHYS_MASK_7
};

#define PROCESSOR_INACTIVE    0
#define PROCESSOR_ACTIVE      1
#define PROCESSOR_SUSPEND     2
#define PROCESSOR_RESUME      3
#define PROCESSOR_DEBUG       4
#define PROCESSOR_SHUTDOWN    5
#define PROCESSOR_IPI         6
#define PROCESSOR_SWITCH      7
#define PROCESSOR_HOLD        8

#define PIC1_DEBUG_MASK    0xFC
#define PIC2_DEBUG_MASK    0xFF

#define  MAX_PICS             3
#define  PIC_0             0x20
#define  PIC_1             0xA0
#define  PIC_2             0x30
#define  MASK_0            0x21
#define  MASK_1            0xA1
#define  MASK_2            0x31

unsigned char irq_control[MAX_PICS] = { PIC_0, PIC_1, PIC_2 };
unsigned char irq_mask[MAX_PICS] = { MASK_0, MASK_1, MASK_2 };
unsigned char mask_value[MAX_PICS] = { 0xF8, 0xFF, 0xFF };

StackFrame ReferenceFrame[MAX_PROCESSORS];
NUMERIC_FRAME npx[MAX_PROCESSORS];

#if defined(CONFIG_SMP)
volatile rlock_t debug_mutex = { SPIN_LOCK_UNLOCKED, -1, 0 };
#else
volatile rlock_t debug_mutex = { -1, 0 };
#endif

atomic_t focusActive;  /* cpus is focus */
atomic_t debuggerActive;  /* cpus in the debugger */
atomic_t debuggerProcessors[MAX_PROCESSORS]; /* cpus in handlers */
atomic_t nmiProcessors[MAX_PROCESSORS]; /* cpus suspended */
atomic_t traceProcessors[MAX_PROCESSORS]; /* focus processor mode */
volatile unsigned long ProcessorHold[MAX_PROCESSORS];
volatile unsigned long ProcessorState[MAX_PROCESSORS];

unsigned char *procState[]={
   "PROCESSOR_INACTIVE", "PROCESSOR_ACTIVE  ", "PROCESSOR_SUSPEND ",
   "PROCESSOR_RESUME  ", "PROCESSOR_DEBUG   ", "PROCESSOR_SHUTDOWN",
   "PROCESSOR_IPI     ", "PROCESSOR_SWITCH  ", "PROCESSOR_HOLD    ",
   "?                 ", "?                 ", "?                 ",
   "?                 ", "?                 ", "?                 ",
   "?                 "
};

/* debugger commands */

DEBUGGER_PARSER backTraceAllPidPE = {
0, 0, backTraceAllPID, backTraceHelp, 0, "BTA", 0, 0,
"display stack backtrace for all processes" , 0 };

DEBUGGER_PARSER backTracePidPE = {
0, 0, backTracePID, backTraceHelp, 0, "BTP", 0, 0,
"display stack backtrace by pid" , 0 };

DEBUGGER_PARSER backTraceStackPE = {
0, 0, backTraceStack, backTraceHelp, 0, "BT", 0, 0,
"display stack backtrace by address" , 0 };

DEBUGGER_PARSER cpuFramePE = {
0, 0, listProcessorFrame, processorCommandHelp, 0, "LR", 0, 0,
"display cpu registers" , 0 };

DEBUGGER_PARSER ProcessorPE = {
0, 0, displayProcessorStatus, displayProcessorStatusHelp, 0, "PROCESSORS", 0, 0,
"display processor status" , 0 };

DEBUGGER_PARSER HPE = {
0, 0, displayDebuggerHelp, displayDebuggerHelpHelp, 0, "HELP", 0, 0,
"this help screen (type HELP <command> for specific help)" , 0 };

DEBUGGER_PARSER HelpPE = {
0, 0, displayDebuggerHelp, displayDebuggerHelpHelp, 0, "H", 0, 0,
"this help screen" , 0 };

DEBUGGER_PARSER clearScreenPE = {
0, 0, clearDebuggerScreen, clearScreenHelp, 0, "CLS", 0, 0,
"clear the screen" , 0 };

DEBUGGER_PARSER asciiTablePE = {
0, 0, displayASCTable, ascTableHelp, 0, "A", 0, 0,
"display ASCII Table" , 0 };

DEBUGGER_PARSER displayToggle1 = {
0, 0, displayToggleAll, displayToggleHelp, 0, ".TOGGLE", 0, 0,
"show all current toggle settings" , 0 };

DEBUGGER_PARSER displayToggle2 = {
0, 0, displayToggleAll, displayToggleHelp, 0, "TOGGLE", 0, 0,
"show all current toggle settings" , 0 };

DEBUGGER_PARSER TBTogglePE = {
0, 0, ProcessTBToggle, displayToggleHelp, 0, ".TB", 0, 0,
"toggle disable breakpoints in user address space (ON | OFF)" , 0 };

DEBUGGER_PARSER TUTogglePE = {
0, 0, ProcessTUToggle, displayToggleHelp, 0, ".TU", 0, 0,
"toggles unasm debug display (ON | OFF)" , 0 };

DEBUGGER_PARSER TDTogglePE = {
0, 0, ProcessTDToggle, displayToggleHelp, 0, ".TD", 0, 0,
"toggles full dereference display (ON | OFF)" , 0 };

DEBUGGER_PARSER TLTogglePE = {
0, 0, ProcessTLToggle, displayToggleHelp, 0, ".TL", 0, 0,
"toggles source line display (ON | OFF)" , 0 };

DEBUGGER_PARSER TGTogglePE = {
0, 0, ProcessTGToggle, displayToggleHelp, 0, ".TG", 0, 0,
"toggles general registers (ON | OFF)" , 0 };

DEBUGGER_PARSER TCTogglePE = {
0, 0, ProcessTCToggle, displayToggleHelp, 0, ".TC", 0, 0,
"toggles control registers (ON | OFF)" , 0 };

DEBUGGER_PARSER TNTogglePE = {
0, 0, ProcessTNToggle, displayToggleHelp, 0, ".TN", 0, 0,
"toggles coprocessor registers (ON | OFF)"  , 0 };

DEBUGGER_PARSER TRTogglePE = {
0, 0, ProcessTRToggle, displayToggleHelp, 0, ".TR", 0, 0,
"toggles display of break reason (ON | OFF)"  , 0 };

DEBUGGER_PARSER TSTogglePE = {
0, 0, ProcessTSToggle, displayToggleHelp, 0, ".TS", 0, 0,
"toggles segment registers (ON | OFF)"  , 0 };

DEBUGGER_PARSER TATogglePE = {
0, 0, ProcessTAToggle, displayToggleHelp, 0, ".TA", 0, 0,
"toggles all registers (ON | OFF)" , 0 };

DEBUGGER_PARSER ToggleUser = {
0, 0, ProcessToggleUser, displayToggleHelp, 0, ".TM", 0, 0,
"toggle memory reads/write to map user space address ranges < PAGE_OFFSET" , 0 };

DEBUGGER_PARSER ReasonPE = {
0, 0, ReasonDisplay, ReasonHelp, 0, ".A", 0, 0,
"display break reason" , 0 };

DEBUGGER_PARSER TTogglePE = {
0, 0, TSSDisplay, TSSDisplayHelp, 0, ".T", 0, 0,
"display task state segment (tss)" , 0 };

DEBUGGER_PARSER versionPE = {
0, 0, DisplayDebuggerVersion, displayDebuggerVersionHelp, 0, ".V", 0, 0,
"display version info" , 0 };

#if defined(CONFIG_MODULES)
DEBUGGER_PARSER lsmodPE1 = {
0, 0, listModules, listModulesHelp, 0, ".M", 0, 0,
"list loaded modules" , 0 };

DEBUGGER_PARSER lsmodPE2 = {
0, 0, listModules, listModulesHelp, 0, "LSMOD", 0, 0,
"list loaded modules" , 0 };

DEBUGGER_PARSER rmmodPE = {
0, 0, unloadModule, listModulesHelp, 0, "RMMOD", 0, 0,
"unload module" , 0 };
#endif

DEBUGGER_PARSER rebootPE = {
0, 0, rebootSystem, rebootSystemHelp, 0, "REBOOT", 0, 0,
"reboot host system" , 0 };

DEBUGGER_PARSER KernelProcessPE1 = {
0, 0, displayKernelProcess, displayKernelProcessHelp, 0, ".P", 0, 0,
"display kernel processes" , 0 };

DEBUGGER_PARSER KernelProcessPE2 = {
0, 0, displayKernelProcess, displayKernelProcessHelp, 0, "PS", 0, 0,
"display kernel processes" , 0 };

/*
DEBUGGER_PARSER SwitchProcess = {
0, 0, switchKernelProcess, displayKernelSwitchProcessHelp, 0, "PID", 0, 0,
"switch running kernel process" , 0 };

DEBUGGER_PARSER SectionPE1 = {
0, 0, displaySections, displaySectionsHelp, 0, ".S", 0, 0,
"display kernel/module sections" , 0 };

DEBUGGER_PARSER SectionPE2 = {
0, 0, displaySections, displaySectionsHelp, 0, "SECTIONS", 0, 0,
"display kernel/module sections" , 0 };
*/

DEBUGGER_PARSER AllSymbolsPE = {
0, 0, displaySymbols, displaySymbolsHelp, 0, "SYMBOL", 0, 0,
"display symbol(s)" , 0 };

DEBUGGER_PARSER SymbolsPE = {
0, 0, displaySymbols, displaySymbolsHelp, 0, ".Z", 0, 0,
"display symbol(s)" , 0 };

DEBUGGER_PARSER ControlPE = {
0, 0, displayControlRegisters, displayRegistersHelp, 0, "RC", 0, 0,
"display control registers" , 0 };

DEBUGGER_PARSER AllPE = {
0, 0, displayAllRegisters, displayRegistersHelp, 0, "RA", 0, 0,
"display all registers" , 0 };

DEBUGGER_PARSER SegmentPE = {
0, 0, displaySegmentRegisters, displayRegistersHelp, 0, "RS", 0, 0,
"display segment registers" , 0 };

DEBUGGER_PARSER NumericPE = {
0, 0, displayNumericRegisters, displayRegistersHelp, 0, "RN", 0, 0,
"display coprocessor/MMX registers" , 0 };

DEBUGGER_PARSER GeneralPE = {
0, 0, displayGeneralRegisters, displayRegistersHelp, 0, "RG", 0, 0,
"display general registers" , 0 };

DEBUGGER_PARSER DefaultPE = {
0, 0, displayDefaultRegisters, displayRegistersHelp, 0, "R", 0, 0,
"display registers for a processor" , 0 };

DEBUGGER_PARSER SearchMemoryBPE = {
0, 0, SearchMemoryB, SearchMemoryHelp, 0, "SB", 0, 0,
"search memory for pattern (bytes)"  , 0 };

DEBUGGER_PARSER SearchMemoryWPE = {
0, 0, SearchMemoryW, SearchMemoryHelp, 0, "SW", 0, 0,
"search memory for pattern (words)"  , 0 };

DEBUGGER_PARSER SearchMemoryDPE = {
0, 0, SearchMemoryD, SearchMemoryHelp, 0, "SD", 0, 0,
"search memory for pattern (dwords)"  , 0 };

DEBUGGER_PARSER ChangeWordPE = {
0, 0, changeWordValue, changeMemoryHelp, 0, "CW", 0, 0,
"change words at address"  , 0 };

DEBUGGER_PARSER ChangeDoublePE = {
0, 0, changeDoubleValue, changeMemoryHelp, 0, "CD", 0, 0,
"change dwords at address"  , 0 };

DEBUGGER_PARSER ChangeBytePE = {
0, 0, changeByteValue, changeMemoryHelp, 0, "CB", 0, 0,
"change bytes at address"  , 0 };

DEBUGGER_PARSER ChangeDefaultPE = {
0, 0, changeDefaultValue, changeMemoryHelp, 0, "C", 0, 0,
"change bytes at address"  , 0 };

DEBUGGER_PARSER CloseSymbolsPE = {
0, 0, displayCloseSymbols, displayCloseHelp, 0, "?", 0, 0,
"display closest symbols to <address>" , 0 };

DEBUGGER_PARSER WalkPE = {
0, 0, debuggerWalkStack, displayDumpHelp, 0, "W", 0, 0,
"display symbols on the stack"  , 0 };

DEBUGGER_PARSER DumpLinkedPE = {
0, 0, debuggerDumpLinkedList, displayDumpHelp, 0, "DL", 0, 0,
"dump linked list"  , 0 };

DEBUGGER_PARSER DumpWordPE = {
0, 0, debuggerDumpWord, displayDumpHelp, 0, "DW", 0, 0,
"dump memory as words"  , 0 };

DEBUGGER_PARSER DumpStackPE = {
0, 0, debuggerDumpStack, displayDumpHelp, 0, "DS", 0, 0,
"dump stack"  , 0 };

DEBUGGER_PARSER DumpDoubleStackPE = {
0, 0, debuggerDumpDoubleStack, displayDumpHelp, 0, "DDS", 0, 0,
"dump stack double word"  , 0 };

DEBUGGER_PARSER DumpQuadStackPE = {
0, 0, debuggerDumpQuadStack, displayDumpHelp, 0, "DQS", 0, 0,
"dump stack quad word"  , 0 };

DEBUGGER_PARSER DumpDoublePE = {
0, 0, debuggerDumpDouble, displayDumpHelp, 0, "DD", 0, 0,
"dump memory as double words" , 0 };

DEBUGGER_PARSER DumpQuadPE = {
0, 0, debuggerDumpQuad, displayDumpHelp, 0, "DQ", 0, 0,
"dump memory as quad words" , 0 };

DEBUGGER_PARSER DumpBytePE = {
0, 0, debuggerDumpByte, displayDumpHelp, 0, "DB", 0, 0,
"dump memory as bytes"  , 0 };

DEBUGGER_PARSER DumpDefaultPE = {
0, 0, debuggerDumpByte, displayDumpHelp, 0, "D", 0, 0,
"dump memory as bytes"  , 0 };

DEBUGGER_PARSER Diss16PE = {
0, 0, processDisassemble16, displayDisassembleHelp, 0, "UU", 0, 0,
"unassemble code (16-bit)" , 0 };

DEBUGGER_PARSER Diss32PE = {
0, 0, processDisassemble32, displayDisassembleHelp, 0, "UX", 0, 0,
"unassemble code (32-bit)" , 0 };

DEBUGGER_PARSER DissAnyPE = {
0, 0, processDisassembleAny, displayDisassembleHelp, 0, "U", 0, 0,
"unassemble code (INTEL)"  , 0 };

DEBUGGER_PARSER IdPE = {
0, 0, processDisassembleATT, displayDisassembleHelp, 0, "ID", 0, 0,
"unassemble code (ATT)"  , 0 };

DEBUGGER_PARSER ProceedPE = {
0, 0, processProceed, executeCommandHelp, 0, "P", 0, 0,
"proceed"  , -1 };

DEBUGGER_PARSER TracePE = {
0, 0, processTrace, executeCommandHelp, 0, "T", 0, 0,
"trace"  , -1 };

DEBUGGER_PARSER SingleStepPE = {
0, 0, processTrace, executeCommandHelp, 0, "S", 0, 0,
"single step"  , -1 };

DEBUGGER_PARSER TraceSSPE = {
0, 0, processTrace, executeCommandHelp, 0, "SS", 0, 0,
"single step"  , -1 };

DEBUGGER_PARSER TraceSSBPE = {
0, 0, processTraceSSB, executeCommandHelp, 0, "SSB", 0, 0,
"single step til branch", -1 };

DEBUGGER_PARSER GPE = {
0, 0, processGo, executeCommandHelp, 0, "G", 0, 0,
"g or g til <address> match"  , -1 };

DEBUGGER_PARSER GoPE = {
0, 0, processGo, executeCommandHelp, 0, "GO", 0, 0,
"go or go til <address> match"  , -1 };

DEBUGGER_PARSER QPE = {
0, 0, processGo, executeCommandHelp, 0, "Q", 0, 0,
"quit debugger until <address> match"  , -1 };

DEBUGGER_PARSER XPE = {
0, 0, processGo, executeCommandHelp, 0, "X", 0, 0,
"exit debugger until <address> match"  , -1 };

DEBUGGER_PARSER BreakProcessorPE = {
0, 0, breakProcessor, processorCommandHelp, 0, "CPU", 0, 0,
"switch processor"  , -1 };

DEBUGGER_PARSER ListProcessorsPE = {
0, 0, listProcessors, processorCommandHelp, 0, "LCPU", 0, 0,
"list processors"  , 0 };

DEBUGGER_PARSER EAXPE = {
0, 0, ChangeEAXRegister, displayEAXHelp, 0, "EAX", 0, -1,
"", 0 };

DEBUGGER_PARSER ORIGEAXPE = {
0, 0, ChangeORIGEAXRegister, displayEAXHelp, 0, "ORGEAX", 0, -1,
"", 0 };

DEBUGGER_PARSER EBXPE = {
0, 0, ChangeEBXRegister, displayEBXHelp, 0, "EBX", 0, -1,
"", 0 };

DEBUGGER_PARSER ECXPE = {
0, 0, ChangeECXRegister, displayECXHelp, 0, "ECX", 0, -1,
"", 0 };

DEBUGGER_PARSER EDXPE = {
0, 0, ChangeEDXRegister, displayEDXHelp, 0, "EDX", 0, -1,
"", 0 };

DEBUGGER_PARSER ESIPE = {
0, 0, ChangeESIRegister, displayESIHelp, 0, "ESI", 0, -1,
"", 0 };

DEBUGGER_PARSER EDIPE = {
0, 0, ChangeEDIRegister, displayEDIHelp, 0, "EDI", 0, -1,
"", 0 };

DEBUGGER_PARSER EBPPE = {
0, 0, ChangeEBPRegister, displayEBPHelp, 0, "EBP", 0, -1,
"", 0 };

DEBUGGER_PARSER ESPPE = {
0, 0, ChangeESPRegister, displayESPHelp, 0, "ESP", 0, -1,
"", 0 };

DEBUGGER_PARSER EIPPE = {
0, 0, ChangeEIPRegister, displayEIPHelp, 0, "EIP", 0, -1,
"", 0 };

#ifdef CONFIG_X86_64
DEBUGGER_PARSER RAXPE = {
0, 0, ChangeRAXRegister, displayRAXHelp, 0, "RAX", 0, -1,
"", 0 };

DEBUGGER_PARSER ORIGRAXPE = {
0, 0, ChangeORIGRAXRegister, displayRAXHelp, 0, "ORGRAX", 0, -1,
"", 0 };

DEBUGGER_PARSER RBXPE = {
0, 0, ChangeRBXRegister, displayRBXHelp, 0, "RBX", 0, -1,
"", 0 };

DEBUGGER_PARSER RCXPE = {
0, 0, ChangeRCXRegister, displayRCXHelp, 0, "RCX", 0, -1,
"", 0 };

DEBUGGER_PARSER RDXPE = {
0, 0, ChangeRDXRegister, displayRDXHelp, 0, "RDX", 0, -1,
"", 0 };

DEBUGGER_PARSER RSIPE = {
0, 0, ChangeRSIRegister, displayRSIHelp, 0, "RSI", 0, -1,
"", 0 };

DEBUGGER_PARSER RDIPE = {
0, 0, ChangeRDIRegister, displayRDIHelp, 0, "RDI", 0, -1,
"", 0 };

DEBUGGER_PARSER RBPPE = {
0, 0, ChangeRBPRegister, displayRBPHelp, 0, "RBP", 0, -1,
"", 0 };

DEBUGGER_PARSER RSPPE = {
0, 0, ChangeRSPRegister, displayRSPHelp, 0, "RSP", 0, -1,
"", 0 };

DEBUGGER_PARSER RIPPE = {
0, 0, ChangeRIPRegister, displayRIPHelp, 0, "RIP", 0, -1,
"", 0 };

DEBUGGER_PARSER R8PE = {
0, 0, ChangeR8Register, displayR8Help, 0, "R8", 0, -1,
"", 0 };

DEBUGGER_PARSER R9PE = {
0, 0, ChangeR9Register, displayR9Help, 0, "R9", 0, -1,
"", 0 };

DEBUGGER_PARSER R10PE = {
0, 0, ChangeR10Register, displayR10Help, 0, "R10", 0, -1,
"", 0 };

DEBUGGER_PARSER R11PE = {
0, 0, ChangeR11Register, displayR11Help, 0, "R11", 0, -1,
"", 0 };

DEBUGGER_PARSER R12PE = {
0, 0, ChangeR12Register, displayR12Help, 0, "R12", 0, -1,
"", 0 };

DEBUGGER_PARSER R13PE = {
0, 0, ChangeR13Register, displayR13Help, 0, "R13", 0, -1,
"", 0 };

DEBUGGER_PARSER R14PE = {
0, 0, ChangeR14Register, displayR14Help, 0, "R14", 0, -1,
"", 0 };

DEBUGGER_PARSER R15PE = {
0, 0, ChangeR15Register, displayR15Help, 0, "R15", 0, -1,
"", 0 };

#endif

DEBUGGER_PARSER CSPE = {
0, 0, ChangeCSRegister, displayCSHelp, 0, "XCS", 0, -1,
"", 0 };

#ifndef CONFIG_X86_64
DEBUGGER_PARSER DSPE = {
0, 0, ChangeDSRegister, displayDSHelp, 0, "XDS", 0, -1,
"", 0 };

DEBUGGER_PARSER ESPE = {
0, 0, ChangeESRegister, displayESHelp, 0, "XES", 0, -1,
"", 0 };
#endif

DEBUGGER_PARSER FSPE = {
0, 0, ChangeFSRegister, displayFSHelp, 0, "XFS", 0, -1,
"", 0 };

DEBUGGER_PARSER GSPE = {
0, 0, ChangeGSRegister, displayGSHelp, 0, "XGS", 0, -1,
"", 0 };

DEBUGGER_PARSER SSPE = {
0, 0, ChangeSSRegister, displaySSHelp, 0, "XSS", 0, -1,
"", 0 };

DEBUGGER_PARSER RFPE = {
0, 0, ChangeRFFlag, displayRFHelp, 0, "RF", 0, -1,
"", 0 };

DEBUGGER_PARSER TFPE = {
0, 0, ChangeTFFlag, displayTFHelp, 0, "TF", 0, -1,
"", 0 };

DEBUGGER_PARSER ZFPE = {
0, 0, ChangeZFFlag, displayZFHelp, 0, "ZF", 0, -1,
"", 0 };

DEBUGGER_PARSER SFPE = {
0, 0, ChangeSFFlag, displaySFHelp, 0, "SF", 0, -1,
"", 0 };

DEBUGGER_PARSER PFPE = {
0, 0, ChangePFFlag, displayPFHelp, 0, "PF", 0, -1,
"", 0 };

DEBUGGER_PARSER CFPE = {
0, 0, ChangeCFFlag, displayCFHelp, 0, "CF", 0, -1,
"", 0 };

DEBUGGER_PARSER OFPE = {
0, 0, ChangeOFFlag, displayOFHelp, 0, "OF", 0, -1,
"", 0 };

DEBUGGER_PARSER IFPE = {
0, 0, ChangeIFFlag, displayIFHelp, 0, "IF", 0, -1,
"", 0 };

DEBUGGER_PARSER IDPE = {
0, 0, ChangeIDFlag, displayIDHelp, 0, "CPUID", 0, -1,
"", 0 };

DEBUGGER_PARSER DFPE = {
0, 0, ChangeDFFlag, displayDFHelp, 0, "DF", 0, -1,
"", 0 };

DEBUGGER_PARSER NTPE = {
0, 0, ChangeNTFlag, displayNTHelp, 0, "NT", 0, -1,
"", 0 };

DEBUGGER_PARSER VMPE = {
0, 0, ChangeVMFlag, displayVMHelp, 0, "VM", 0, -1,
"", 0 };

DEBUGGER_PARSER VIFPE = {
0, 0, ChangeVIFFlag, displayVIFHelp, 0, "VIF", 0, -1,
"", 0 };

DEBUGGER_PARSER VIPPE = {
0, 0, ChangeVIPFlag, displayVIPHelp, 0, "VIP", 0, -1,
"", 0 };

DEBUGGER_PARSER AFPE = {
0, 0, ChangeAFFlag, displayAFHelp, 0, "AF", 0, -1,
"", 0 };

DEBUGGER_PARSER ACPE = {
0, 0, ChangeACFlag, displayACHelp, 0, "AC", 0, -1,
"", 0 };

DEBUGGER_PARSER MTRRPE = {
0, 0, DisplayMTRRRegisters, displayMTRRHelp, 0, "MTRR", 0, 0,
"display memory type range registers" , 0 };

#if defined(CONFIG_SMP) && !defined(CONFIG_X86_ELAN)
DEBUGGER_PARSER APIC_PE = {
0, 0, displayAPICInfo, displayAPICHelp, 0, "APIC", 0, 0,
"display local apic registers", 0 };

DEBUGGER_PARSER IOAPIC_PE = {
0, 0, displayIOAPICInfo, displayIOAPICHelp, 0, "IOAPIC", 0, 0,
"display io apic registers", 0 };

DEBUGGER_PARSER NMIProcessorPE = {
0, 0, nmiProcessor, processorCommandHelp, 0, "NMI", 0, 0,
"nmi processor (WARNING: See 'help nmi' for instructions)", -1 };
#endif

DEBUGGER_PARSER GDTPE = {
0, 0, displayGDT, displayGDTHelp, 0, ".G", 0, 0,
"display global descriptor table" , 0 };

DEBUGGER_PARSER IDTPE = {
0, 0, displayIDT, displayIDTHelp, 0, ".I", 0, 0,
"display interrupt descriptor table"  , 0 };

DEBUGGER_PARSER EvaluatePE = {
0, 0, evaluateExpression, evaluateExpressionHelp, 0, ".E", 0, 0,
"evaluate expression (help .e)"  , 0 };

DEBUGGER_PARSER InputWordPE = {
0, 0, inputWordPort, portCommandHelp, 0, "IW", 0, 0,
"input word from port" , 0 };

DEBUGGER_PARSER InputDoublePE = {
0, 0, inputDoublePort, portCommandHelp, 0, "IL", 0, 0,
"input double word from port" , 0 };

DEBUGGER_PARSER InputBytePE = {
0, 0, inputBytePort, portCommandHelp, 0, "IB", 0, 0,
"input byte from port" , 0 };

DEBUGGER_PARSER InputPE = {
0, 0, inputPort, portCommandHelp, 0, "I", 0, 0,
"input byte from port" , 0 };

DEBUGGER_PARSER OutputWordPE = {
0, 0, outputWordPort, portCommandHelp, 0, "OW", 0, 0,
"output word to port" , 0 };

DEBUGGER_PARSER OutputDoublePE = {
0, 0, outputDoublePort, portCommandHelp, 0, "OL", 0, 0,
"output double word to port" , 0 };

DEBUGGER_PARSER OutputBytePE = {
0, 0, outputBytePort, portCommandHelp, 0, "OB", 0, 0,
"output byte to port" , 0 };

DEBUGGER_PARSER OutputPE = {
0, 0, outputPort, portCommandHelp, 0, "O", 0, 0,
"output byte to port" , 0 };

DEBUGGER_PARSER BreakClearAllPE = {
0, 0, breakpointClearAll, breakpointCommandHelp, 0, "BCA", 0, 0,
"clear all breakpoints" , 0 };

DEBUGGER_PARSER BreakClearPE = {
0, 0, breakpointClear, breakpointCommandHelp, 0, "BC", 0, 0,
"clear breakpoint" , 0 };

DEBUGGER_PARSER BreakMaskPE = {
0, 0, breakpointMask, breakpointCommandHelp, 0, "BM", 0, 0,
"mask breaks for specific processor" , 0 };

DEBUGGER_PARSER BW1PE = {
0, 0, breakpointWord1, breakpointCommandHelp, 0, "BW1", 0, -1,
"" , 0 };

DEBUGGER_PARSER BW2PE = {
0, 0, breakpointWord2, breakpointCommandHelp, 0, "BW2", 0, -1,
"" , 0 };

DEBUGGER_PARSER BW4PE = {
0, 0, breakpointWord4, breakpointCommandHelp, 0, "BW4", 0, -1,
"" , 0 };

DEBUGGER_PARSER BWPE = {
0, 0, breakpointWord, breakpointCommandHelp, 0, "BW", 0, 0,
"set write only breakpoint #=1,2 or 4 byte len" , 0 };

DEBUGGER_PARSER BR1PE = {
0, 0, breakpointRead1, breakpointCommandHelp, 0, "BR1", 0, -1,
"", 0 };

DEBUGGER_PARSER BR2PE = {
0, 0, breakpointRead2, breakpointCommandHelp, 0, "BR2", 0, -1,
"", 0 };

DEBUGGER_PARSER BR4PE = {
0, 0, breakpointRead4, breakpointCommandHelp, 0, "BR4", 0, -1,
"", 0 };

DEBUGGER_PARSER BRPE = {
0, 0, breakpointRead, breakpointCommandHelp, 0, "BR", 0, 0,
"set read/write breakpoint #=1,2 or 4 byte len" , 0 };

DEBUGGER_PARSER BI1PE = {
0, 0, breakpointIO1, breakpointCommandHelp, 0, "BI1", 0, -1,
"", 0 };

DEBUGGER_PARSER BI2PE = {
0, 0, breakpointIO2, breakpointCommandHelp, 0, "BI2", 0, -1,
"", 0 };

DEBUGGER_PARSER BI4PE = {
0, 0, breakpointIO4, breakpointCommandHelp, 0, "BI4", 0, -1,
"", 0 };

DEBUGGER_PARSER BIPE = {
0, 0, breakpointIO, breakpointCommandHelp, 0, "BI", 0, 0,
"set io address breakpoint #=1,2 or 4 byte len"  , 0 };

DEBUGGER_PARSER breakpointExecutePE = {
0, 0, breakpointExecute, breakpointCommandHelp, 0, "B", 0, 0,
"display all/set execute breakpoint" , 0 };

DEBUGGER_PARSER breakShowTemp = {
0, 0, breakpointShowTemp, breakpointCommandHelp, 0, "BST", 0, 0,
"displays temporary breakpoints (proceed/go)" , 0 };

DEBUGGER_PARSER breakTimer = {
0, 0, timerBreakpoint, timedBreakpointHelp, 0, "ADDTIMER", 0, 0,
"add a debug timer event" , 0 };

DEBUGGER_PARSER breakTimerClear = {
0, 0, timerBreakpointClear, timedBreakpointHelp, 0, "DELTIMER", 0, 0,
"delete a debug timer event" , 0 };

/* interactive debugger accelerators */

ACCELERATOR traceSSBACC = {
0, 0, processTraceSSBACC, 0, 0, K_F6, 0,
"F6 - Trace/Single Step til Branch" };

ACCELERATOR traceACC = {
0, 0, processTraceACC, 0, 0, K_F7, 0,
"F7 - Trace/Single Step" };

ACCELERATOR proceedACC = {
0, 0, processProceedACC, 0, 0, K_F8, 0,
"F8 - Proceed" };

ACCELERATOR goACC = {
0, 0, processGoACC, 0, 0, K_F9, 0,
"F9 - Go" };

ACCELERATOR enterACC = { /* this accelerator handles repeat command */
0, 0, enterKeyACC, 0, 0, 13, 0,   /* processing */
"Enter - Execute or Repeat a Command" };

unsigned char *lastDumpAddress;
unsigned char *lastLinkAddress;
unsigned long lastUnasmAddress;
unsigned long displayLength;
unsigned long lastCommand;
unsigned long lastCommandEntry;
unsigned char lastDebugCommand[100] = {""};
unsigned long lastDisplayLength;
unsigned char debugCommand[100] = {""};
unsigned long nextUnasmAddress;
unsigned long pic1Value;
unsigned long pic2Value;
unsigned long BreakReserved[4];
unsigned long BreakPoints[4];
unsigned long BreakType[4];
unsigned long BreakLength[4];
unsigned long BreakTemp[4];
unsigned long BreakGo[4];
unsigned long BreakProceed[4];
unsigned long BreakMask[MAX_PROCESSORS];
StackFrame *CurrentFrame[MAX_PROCESSORS];
unsigned long NestedInterrupts[MAX_PROCESSORS];
unsigned long ConditionalBreakpoint[4];
unsigned char BreakCondition[4][256];
StackFrame lastStackFrame;
unsigned long lastCR0;
unsigned long lastCR2;
unsigned long lastCR4;
unsigned long VirtualDR6;
unsigned long CurrentDR7;
unsigned long CurrentDR6[MAX_PROCESSORS];
unsigned long repeatCommand;
unsigned long totalLines;
unsigned long debuggerInitialized;
unsigned long ssbmode;

void mdb_breakpoint(void) 
{
   __asm__ __volatile__ ("int $0x03");
}

void MDBInitializeDebugger(void)
{
   register unsigned long i;
   extern void InitializeDebuggerRegisters(void);
   extern unsigned long AddAccelRoutine(ACCELERATOR *);

   lastCommand = 0;
   lastCommandEntry = 0;
   lastDisplayLength = 0;

   for (i = 0; i < MAX_PROCESSORS; i++)
   {
      BreakMask[i] = 0;
      ProcessorHold[i] = 0;
      ProcessorState[i] = 0;
   }

   for (i = 0; i < 4; i++)
   {
      BreakReserved[i] = 0;
      BreakPoints[i] = 0;
      BreakType[i] = 0;
      BreakLength[i] = 0;
      BreakProceed[i] = 0;
      BreakGo[i] = 0;
      BreakTemp[i] = 0;
      ConditionalBreakpoint[i] = 0;
      BreakCondition[i][0] = '\0';
   }

   InitializeDebuggerRegisters();

   AddDebuggerCommandParser(&ReasonPE);
   AddDebuggerCommandParser(&backTraceAllPidPE);
   AddDebuggerCommandParser(&backTracePidPE);
   AddDebuggerCommandParser(&backTraceStackPE);
   AddDebuggerCommandParser(&cpuFramePE);
   AddDebuggerCommandParser(&ProcessorPE);
   AddDebuggerCommandParser(&HPE);
   AddDebuggerCommandParser(&HelpPE);
   AddDebuggerCommandParser(&clearScreenPE);
   AddDebuggerCommandParser(&asciiTablePE);
   AddDebuggerCommandParser(&displayToggle1);
   AddDebuggerCommandParser(&displayToggle2);
   AddDebuggerCommandParser(&TBTogglePE);
   AddDebuggerCommandParser(&TUTogglePE);
   AddDebuggerCommandParser(&TDTogglePE);
   AddDebuggerCommandParser(&TLTogglePE);
   AddDebuggerCommandParser(&TGTogglePE);
   AddDebuggerCommandParser(&TCTogglePE);
   AddDebuggerCommandParser(&TNTogglePE);
   AddDebuggerCommandParser(&TRTogglePE);
   AddDebuggerCommandParser(&TSTogglePE);
   AddDebuggerCommandParser(&TATogglePE);
   AddDebuggerCommandParser(&ToggleUser);
   AddDebuggerCommandParser(&TTogglePE);
   AddDebuggerCommandParser(&versionPE);
   AddDebuggerCommandParser(&rebootPE);
   AddDebuggerCommandParser(&KernelProcessPE1);
   AddDebuggerCommandParser(&KernelProcessPE2);
/*
   AddDebuggerCommandParser(&SectionPE1);
   AddDebuggerCommandParser(&SectionPE2);
*/
   AddDebuggerCommandParser(&AllSymbolsPE);
   AddDebuggerCommandParser(&SymbolsPE);

#if defined(CONFIG_MODULES)
   AddDebuggerCommandParser(&lsmodPE1);
   AddDebuggerCommandParser(&lsmodPE2);
   AddDebuggerCommandParser(&rmmodPE);
#endif

   AddDebuggerCommandParser(&ControlPE);
   AddDebuggerCommandParser(&AllPE);
   AddDebuggerCommandParser(&SegmentPE);
   AddDebuggerCommandParser(&NumericPE);
   AddDebuggerCommandParser(&GeneralPE);
   AddDebuggerCommandParser(&DefaultPE);
   AddDebuggerCommandParser(&SearchMemoryBPE);
   AddDebuggerCommandParser(&SearchMemoryWPE);
   AddDebuggerCommandParser(&SearchMemoryDPE);
   AddDebuggerCommandParser(&ChangeWordPE);
   AddDebuggerCommandParser(&ChangeDoublePE);
   AddDebuggerCommandParser(&ChangeBytePE);
   AddDebuggerCommandParser(&ChangeDefaultPE);
   AddDebuggerCommandParser(&CloseSymbolsPE);
   AddDebuggerCommandParser(&WalkPE);
   AddDebuggerCommandParser(&DumpLinkedPE);
   AddDebuggerCommandParser(&DumpWordPE);
   AddDebuggerCommandParser(&DumpStackPE);
   AddDebuggerCommandParser(&DumpDoubleStackPE);
   AddDebuggerCommandParser(&DumpDoublePE);
   AddDebuggerCommandParser(&DumpQuadStackPE);
   AddDebuggerCommandParser(&DumpQuadPE);
   AddDebuggerCommandParser(&DumpBytePE);
   AddDebuggerCommandParser(&DumpDefaultPE);
   AddDebuggerCommandParser(&Diss16PE);
   AddDebuggerCommandParser(&Diss32PE);
   AddDebuggerCommandParser(&DissAnyPE);
   AddDebuggerCommandParser(&IdPE);
   AddDebuggerCommandParser(&ProceedPE);
   AddDebuggerCommandParser(&TracePE);
   AddDebuggerCommandParser(&SingleStepPE);
   AddDebuggerCommandParser(&TraceSSPE);
   AddDebuggerCommandParser(&TraceSSBPE);
   AddDebuggerCommandParser(&GPE);
   AddDebuggerCommandParser(&GoPE);
   AddDebuggerCommandParser(&QPE);
   AddDebuggerCommandParser(&XPE);
   AddDebuggerCommandParser(&BreakProcessorPE);
   AddDebuggerCommandParser(&ListProcessorsPE);
#ifdef CONFIG_X86_64
   AddDebuggerCommandParser(&RAXPE);
   AddDebuggerCommandParser(&ORIGRAXPE);
   AddDebuggerCommandParser(&RBXPE);
   AddDebuggerCommandParser(&RCXPE);
   AddDebuggerCommandParser(&RDXPE);
   AddDebuggerCommandParser(&RSIPE);
   AddDebuggerCommandParser(&RDIPE);
   AddDebuggerCommandParser(&RBPPE);
   AddDebuggerCommandParser(&RSPPE);
   AddDebuggerCommandParser(&RIPPE);
   AddDebuggerCommandParser(&R8PE);
   AddDebuggerCommandParser(&R9PE);
   AddDebuggerCommandParser(&R10PE);
   AddDebuggerCommandParser(&R11PE);
   AddDebuggerCommandParser(&R12PE);
   AddDebuggerCommandParser(&R13PE);
   AddDebuggerCommandParser(&R14PE);
   AddDebuggerCommandParser(&R15PE);
#endif
   AddDebuggerCommandParser(&EAXPE);
   AddDebuggerCommandParser(&ORIGEAXPE);
   AddDebuggerCommandParser(&EBXPE);
   AddDebuggerCommandParser(&ECXPE);
   AddDebuggerCommandParser(&EDXPE);
   AddDebuggerCommandParser(&ESIPE);
   AddDebuggerCommandParser(&EDIPE);
   AddDebuggerCommandParser(&EBPPE);
   AddDebuggerCommandParser(&ESPPE);
   AddDebuggerCommandParser(&EIPPE);
   AddDebuggerCommandParser(&CSPE);
#ifndef CONFIG_X86_64
   AddDebuggerCommandParser(&DSPE);
   AddDebuggerCommandParser(&ESPE);
#endif
   AddDebuggerCommandParser(&FSPE);
   AddDebuggerCommandParser(&GSPE);
   AddDebuggerCommandParser(&SSPE);
   AddDebuggerCommandParser(&RFPE);
   AddDebuggerCommandParser(&TFPE);
   AddDebuggerCommandParser(&ZFPE);
   AddDebuggerCommandParser(&SFPE);
   AddDebuggerCommandParser(&PFPE);
   AddDebuggerCommandParser(&CFPE);
   AddDebuggerCommandParser(&OFPE);
   AddDebuggerCommandParser(&IFPE);
   AddDebuggerCommandParser(&IDPE);
   AddDebuggerCommandParser(&DFPE);
   AddDebuggerCommandParser(&NTPE);
   AddDebuggerCommandParser(&VMPE);
   AddDebuggerCommandParser(&VIFPE);
   AddDebuggerCommandParser(&VIPPE);
   AddDebuggerCommandParser(&AFPE);
   AddDebuggerCommandParser(&ACPE);
   AddDebuggerCommandParser(&MTRRPE);
#if defined(CONFIG_SMP) && !defined(CONFIG_X86_ELAN)
   AddDebuggerCommandParser(&APIC_PE);
   AddDebuggerCommandParser(&IOAPIC_PE);
   AddDebuggerCommandParser(&NMIProcessorPE);
#endif
   AddDebuggerCommandParser(&GDTPE);
   AddDebuggerCommandParser(&IDTPE);
   AddDebuggerCommandParser(&EvaluatePE);
   AddDebuggerCommandParser(&InputWordPE);
   AddDebuggerCommandParser(&InputDoublePE);
   AddDebuggerCommandParser(&InputBytePE);
   AddDebuggerCommandParser(&InputPE);
   AddDebuggerCommandParser(&OutputWordPE);
   AddDebuggerCommandParser(&OutputDoublePE);
   AddDebuggerCommandParser(&OutputBytePE);
   AddDebuggerCommandParser(&OutputPE);
   AddDebuggerCommandParser(&BreakClearAllPE);
   AddDebuggerCommandParser(&BreakClearPE);
   AddDebuggerCommandParser(&BreakMaskPE);
   AddDebuggerCommandParser(&BW1PE);
   AddDebuggerCommandParser(&BW2PE);
   AddDebuggerCommandParser(&BW4PE);
   AddDebuggerCommandParser(&BWPE);
   AddDebuggerCommandParser(&BR1PE);
   AddDebuggerCommandParser(&BR2PE);
   AddDebuggerCommandParser(&BR4PE);
   AddDebuggerCommandParser(&BRPE);
   AddDebuggerCommandParser(&BI1PE);
   AddDebuggerCommandParser(&BI2PE);
   AddDebuggerCommandParser(&BI4PE);
   AddDebuggerCommandParser(&BIPE);
   AddDebuggerCommandParser(&breakpointExecutePE);
   AddDebuggerCommandParser(&breakShowTemp);
   AddDebuggerCommandParser(&breakTimer);
   AddDebuggerCommandParser(&breakTimerClear);

   AddAccelRoutine(&traceSSBACC);
   AddAccelRoutine(&traceACC);
   AddAccelRoutine(&proceedACC);
   AddAccelRoutine(&goACC);
   AddAccelRoutine(&enterACC);

   debuggerInitialized = 1;
   return;
}

void MDBClearDebuggerState(void)
{
   extern void ClearDebuggerRegisters(void);

   ClearDebuggerRegisters();
   debuggerInitialized = 0;
   return;
}


void ClearTempBreakpoints(void)
{
   register unsigned long i;

   for (i = 0; i < 4; i++)
   {
      if (BreakTemp[i])
      {
	 BreakTemp[i] = 0;
	 BreakReserved[i] = 0;
	 BreakPoints[i] = 0;
	 BreakType[i] = 0;
	 BreakLength[i] = 0;
	 BreakGo[i] = 0;
	 BreakProceed[i] = 0;
      }
   }
   SetDebugRegisters();
   return;
}

unsigned long ValidBreakpoint(unsigned long address)
{

   register unsigned long i;

   for (i = 0; i < 4; i++)
   {
      if (!BreakTemp[i])
	 if (BreakPoints[i] == address)
	    return 1;
   }
   return 0;

}

unsigned long GetIP(StackFrame *stackFrame)
{
    return (unsigned long)(stackFrame->tIP);
}

unsigned long GetStackAddress(StackFrame *stackFrame)
{
    return (unsigned long)(stackFrame->tSP);
}

unsigned long GetStackSegment(StackFrame *stackFrame)
{
    return (unsigned long)(stackFrame->tSS);
}

unsigned long SSBUpdate(StackFrame *stackFrame, unsigned long processor)
{
    if (!ssbmode)
       return 0;

    if (jmp_active)
    {
       ssbmode = 0;
       return 0;
    }

    lastCommand = 'T';
    strcpy(debugCommand, "SSB");
    lastCR0 = ReadCR0();
    lastCR2 = ReadCR2();
    lastCR4 = ReadCR4();
    memmove((void *)&lastStackFrame, stackFrame,
		     sizeof(StackFrame));

    stackFrame->tSystemFlags |= (SINGLE_STEP | RESUME);
    atomic_inc(&focusActive);
    atomic_inc(&traceProcessors[get_processor_id()]);
    return -1;
}

/* F6 */

unsigned long processTraceSSBACC(unsigned long key, void *p, ACCELERATOR *accel)
{
     register StackFrame *stackFrame = p;

     DBGPrint("\n");
     ssbmode = 1;
     lastCommand = 'T';
     strcpy(debugCommand, "SSB");
     lastCR0 = ReadCR0();
     lastCR2 = ReadCR2();
     lastCR4 = ReadCR4();
     memmove((void *)&lastStackFrame, stackFrame, sizeof(StackFrame));

     stackFrame->tSystemFlags |= (SINGLE_STEP | RESUME);

     /* set as focus processor for trace, ssb, or proceed */
     atomic_inc(&focusActive);
     atomic_inc(&traceProcessors[get_processor_id()]);
     return -1;
}

/* F8 */

unsigned long processProceedACC(unsigned long key, void *p, ACCELERATOR *accel)
{
     register StackFrame *stackFrame = p;
     register unsigned long i;

     ssbmode = 0;
     DBGPrint("\n");
     if (needs_proceed)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      BreakReserved[i] = 1;
	      BreakPoints[i] = nextUnasmAddress;
	      BreakType[i] = BREAK_EXECUTE;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      BreakTemp[i] = 1;
	      BreakProceed[i] = 1;
	      SetDebugRegisters();
	      lastCommand = 'P';
              strcpy(debugCommand, "P");
	      lastCR0 = ReadCR0();
	      lastCR2 = ReadCR2();
	      lastCR4 = ReadCR4();
	      memmove((void *)&lastStackFrame, stackFrame,
		      sizeof(StackFrame));

	      stackFrame->tSystemFlags &= ~SINGLE_STEP;
	      stackFrame->tSystemFlags |= RESUME;

              /* set as focus processor for trace, ssb, or proceed */
              atomic_inc(&focusActive);
              atomic_inc(&traceProcessors[get_processor_id()]);
	      return -1;
	   }
	}
	DBGPrint("\nNo breakpoint available for Proceed, (single step) instead");
     }
     lastCommand = 'P';
     strcpy(debugCommand, "P");
     lastCR0 = ReadCR0();
     lastCR2 = ReadCR2();
     lastCR4 = ReadCR4();
     memmove((void *)&lastStackFrame, stackFrame,
	       sizeof(StackFrame));

     stackFrame->tSystemFlags |= (SINGLE_STEP | RESUME);

     /* set as focus processor for trace, ssb, or proceed */
     atomic_inc(&focusActive);
     atomic_inc(&traceProcessors[get_processor_id()]);
     return -1;
}

/* F7 */

unsigned long processTraceACC(unsigned long key, void *p, ACCELERATOR *accel)
{
     register StackFrame *stackFrame = p;

     ssbmode = 0;
     DBGPrint("\n");
     lastCommand = 'T';
     strcpy(debugCommand, "T");
     lastCR0 = ReadCR0();
     lastCR2 = ReadCR2();
     lastCR4 = ReadCR4();
     memmove((void *)&lastStackFrame, stackFrame,
		     sizeof(StackFrame));

     stackFrame->tSystemFlags |= (SINGLE_STEP | RESUME);

     /* set as focus processor for trace, ssb, or proceed */
     atomic_inc(&focusActive);
     atomic_inc(&traceProcessors[get_processor_id()]);
     return -1;
}

/* F9 */

unsigned long processGoACC(unsigned long key, void *p, ACCELERATOR *accel)
{
     register StackFrame *stackFrame = p;

     ssbmode = 0;
     DBGPrint("\n");
     ClearTempBreakpoints();
     lastCommand = 'G';
     lastCR0 = ReadCR0();
     lastCR2 = ReadCR2();
     lastCR4 = ReadCR4();
     memmove((void *)&lastStackFrame, stackFrame, sizeof(StackFrame));

     stackFrame->tSystemFlags &= ~SINGLE_STEP;
     stackFrame->tSystemFlags |= RESUME;
     return -1;

}

unsigned long executeCommandHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("t                        - trace\n");
    DBGPrint("s                        - single step\n");
    DBGPrint("ss                       - single step\n");
    DBGPrint("ssb                      - single step til branch\n");
    DBGPrint("p                        - proceed\n");
    DBGPrint("g or g <address>         - go\n");
    DBGPrint("go or go <address>       - go\n");
    DBGPrint("q or q <address>         - quit\n");
    DBGPrint("x or x <address>         - exit\n");
    DBGPrint("F7                       - trace\n");
    DBGPrint("F8                       - proceed\n");
    DBGPrint("F9                       - go\n");
    DBGPrint("\n");
    return 1;
}

/* P */

unsigned long processProceed(unsigned char *cmd,
		    StackFrame *stackFrame, unsigned long Exception,
		    DEBUGGER_PARSER *parser)
{
     register unsigned long i;

     ssbmode = 0;
     if (needs_proceed)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      BreakReserved[i] = 1;
	      BreakPoints[i] = nextUnasmAddress;
	      BreakType[i] = BREAK_EXECUTE;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      BreakTemp[i] = 1;
	      BreakProceed[i] = 1;
	      SetDebugRegisters();
	      lastCommand = 'P';
	      lastCR0 = ReadCR0();
	      lastCR2 = ReadCR2();
	      lastCR4 = ReadCR4();
	      memmove((void *)&lastStackFrame, stackFrame,
		      sizeof(StackFrame));

	      stackFrame->tSystemFlags &= ~SINGLE_STEP;
	      stackFrame->tSystemFlags |= RESUME;

              /* set as focus processor for trace, ssb, or proceed */
              atomic_inc(&focusActive);
              atomic_inc(&traceProcessors[get_processor_id()]);
	      return -1;
	   }
	}
	DBGPrint("\nNo breakpoint available for Proceed, (single step) instead");
     }
     lastCommand = 'P';
     lastCR0 = ReadCR0();
     lastCR2 = ReadCR2();
     lastCR4 = ReadCR4();
     memmove((void *)&lastStackFrame, stackFrame,
	       sizeof(StackFrame));

     stackFrame->tSystemFlags |= (SINGLE_STEP | RESUME);

     /* set as focus processor for trace, ssb, or proceed */
     atomic_inc(&focusActive);
     atomic_inc(&traceProcessors[get_processor_id()]);
     return -1;

}

/* SSB */

unsigned long processTraceSSB(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     DBGPrint("\n");
     ssbmode = 1;
     lastCommand = 'T';
     lastCR0 = ReadCR0();
     lastCR2 = ReadCR2();
     lastCR4 = ReadCR4();
     memmove((void *)&lastStackFrame, stackFrame,
		     sizeof(StackFrame));

     stackFrame->tSystemFlags |= (SINGLE_STEP | RESUME);

     /* set as focus processor for trace, ssb, or proceed */
     atomic_inc(&focusActive);
     atomic_inc(&traceProcessors[get_processor_id()]);
     return -1;

}


/* T */

unsigned long processTrace(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     ssbmode = 0;
     lastCommand = 'T';
     lastCR0 = ReadCR0();
     lastCR2 = ReadCR2();
     lastCR4 = ReadCR4();
     memmove((void *)&lastStackFrame, stackFrame,
		     sizeof(StackFrame));

     stackFrame->tSystemFlags |= (SINGLE_STEP | RESUME);

     /* set as focus processor for trace, ssb, or proceed */
     atomic_inc(&focusActive);
     atomic_inc(&traceProcessors[get_processor_id()]);
     return -1;

}

/* G */

unsigned long processGo(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     register unsigned long address;
     unsigned long valid;
     register unsigned long i;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     ssbmode = 0;
     ClearTempBreakpoints();
     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_EXECUTE;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      BreakTemp[i] = 1;
	      BreakGo[i] = 1;
	      SetDebugRegisters();
	      DBGPrint("\n");
	      lastCommand = 'G';
	      lastCR0 = ReadCR0();
	      lastCR2 = ReadCR2();
	      lastCR4 = ReadCR4();
	      memmove((void *)&lastStackFrame, stackFrame,
			      sizeof(StackFrame));

	      stackFrame->tSystemFlags &= ~SINGLE_STEP;
	      stackFrame->tSystemFlags |= RESUME;
	      return -1;
	   }
	}
     }
     else
     {
	DBGPrint("\n");
	lastCommand = 'G';
	lastCR0 = ReadCR0();
	lastCR2 = ReadCR2();
	lastCR4 = ReadCR4();
	memmove((void *)&lastStackFrame, stackFrame,
			sizeof(StackFrame));

	stackFrame->tSystemFlags &= ~SINGLE_STEP;
	stackFrame->tSystemFlags |= RESUME;
	return -1;
     }
     DBGPrint("no breakpoint available for GO\n");
     return 1;

}

unsigned long processorCommandHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("lcpu                     - list processors\n");
    DBGPrint("cpu [p#]                 - switch processor\n");
    DBGPrint("lr  [p#]                 - display processor registers\n");
#if defined(CONFIG_SMP) && !defined(CONFIG_X86_ELAN)
    DBGPrint("nmi [p#]                 - deliver hard nmi exception\n\n");
    DBGPrint("The 'nmi [p#]' command will deliver a level triggered\n");
    DBGPrint("NMI hardware interrupt to a specified processor and system\n");
    DBGPrint("recovery may not be possible in all cases after issuing this\n");
    DBGPrint("command.  This command is used to trigger debugger\n");
    DBGPrint("entry on a hard hung processor.\n\n");
    DBGPrint("If you just want to switch processors with a soft NMI\n");
    DBGPrint("exception (which is typically recoverable), then use the\n");
    DBGPrint("'cpu [p#]' command instead.  Only use the 'nmi [p#]' command\n");
    DBGPrint("for situations where you need to trigger a hardware NMI to\n");
    DBGPrint("force a hung processor to enter the debugger.\n");
#endif
    return 1;
}

/* CPU */

unsigned long breakProcessor(unsigned char *cmd,
		     StackFrame *stackFrame, unsigned long Exception,
		     DEBUGGER_PARSER *parser)
{
     register unsigned long cpunum, cpu = get_processor_id();
     unsigned long valid, i;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     cpunum = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
        if (cpunum == cpu)
        {
           DBGPrint("debugger already running on processor %d\n", (int)cpunum);
           return 1;
        }

	if ((cpunum > MAX_PROCESSORS) || !(cpu_online(cpunum)))
        {
	   DBGPrint("invalid processor specified\n");
           return 1;
        }

        for (i = 0; i < MAX_PROCESSORS; i++)
        {
           if (cpu_online(i))
           {
	      if (i == cpunum)
              {
	         ProcessorState[i] = PROCESSOR_SWITCH;
                 ProcessorHold[cpu] = 1;
                 barrier();
                 break;
              }
	   }
        }
	DBGPrint("\n");
	lastCommand = 'G';
	lastCR0 = ReadCR0();
	lastCR2 = ReadCR2();
	lastCR4 = ReadCR4();
	memmove((void *)&lastStackFrame, stackFrame,
			   sizeof(StackFrame));
	return -1;
     }
     else
     {
	DBGPrint("no target processor specified\n");
        DBGPrint("Current Processor: %d\n", get_processor_id());
        DBGPrint("Active Processors: ");

        for (i = 0; i < MAX_PROCESSORS; i++)
        {
           if (cpu_online(i))
           {
	      if (i)
                 DBGPrint(", ");

	      DBGPrint("%d", i);
	   }
        }
        DBGPrint("\n");
     }
     return 1;

}

#if defined(CONFIG_SMP) && !defined(CONFIG_X86_ELAN)
/* NMI */

unsigned long nmiProcessor(unsigned char *cmd,
		     StackFrame *stackFrame, unsigned long Exception,
		     DEBUGGER_PARSER *parser)
{
     register unsigned long cpunum, cpu = get_processor_id();
     unsigned long valid, i;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     cpunum = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
        if (cpunum == cpu)
        {
           DBGPrint("debugger already running on processor %d\n", (int)cpunum);
           return 1;
        }

	if ((cpunum > MAX_PROCESSORS) || !(cpu_online(cpunum)))
        {
	   DBGPrint("invalid processor specified\n");
           return 1;
        }

        for (i = 0; i < MAX_PROCESSORS; i++)
        {
           if (cpu_online(i))
           {
	      if (i == cpunum)
              {
	         ProcessorState[i] = PROCESSOR_SWITCH;
                 ProcessorHold[cpu] = 1;
                 barrier();
//                 apic_directed_nmi(i);
                 break;
              }
	   }
        }
	DBGPrint("\n");
	lastCommand = 'G';
	lastCR0 = ReadCR0();
	lastCR2 = ReadCR2();
	lastCR4 = ReadCR4();
	memmove((void *)&lastStackFrame, stackFrame,
			   sizeof(StackFrame));
	return -1;
     }
     else
     {
	DBGPrint("no target processor specified\n");
        DBGPrint("Current Processor: %d\n", get_processor_id());
        DBGPrint("Active Processors: ");

        for (i = 0; i < MAX_PROCESSORS; i++)
        {
           if (cpu_online(i))
           {
	      if (i)
                 DBGPrint(", ");

	      DBGPrint("%d", i);
	   }
        }
        DBGPrint("\n");
     }
     return 1;

}
#endif

/* LR */

extern  StackFrame CurrentStackFrame[MAX_PROCESSORS];

unsigned long listProcessorFrame(unsigned char *cmd,
			StackFrame *stackFrame, unsigned long Exception,
			DEBUGGER_PARSER *parser)
{
     unsigned long valid, pnum;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     pnum = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid && (pnum < MAX_PROCESSORS) && (cpu_online(pnum)))
     {
	DBGPrint("Processor Frame %d -> (%X)\n", pnum,
                 &CurrentStackFrame[pnum]);
	DisplayTSS((StackFrame *)&CurrentStackFrame[pnum]);
     }
     else
	DBGPrint("invalid processor frame\n");

     return 1;

}

/* .TA */

unsigned long ProcessTAToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     if (general_toggle)
     {
        general_toggle = 0;
        control_toggle = 0;
        segment_toggle = 0;
     }
     else
     {
        general_toggle = 1;
        control_toggle = 1;
        segment_toggle = 1;
     }

     DBGPrint("general registers (%s) \n", general_toggle ? "ON" : "OFF");
     DBGPrint("control registers (%s) \n", control_toggle ? "ON" : "OFF");
     DBGPrint("segment registers (%s) \n", segment_toggle ? "ON" : "OFF");
     return 1;

}

unsigned long TSSDisplayHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint(".t <address>             - display task state regs\n");
    return 1;
}

/* .T */

unsigned long TSSDisplay(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
	DisplayTSS(stackFrame);
     else
	DisplayTSS((StackFrame *) address);

     return 1;
}

unsigned long displayRegistersHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("r                        - display registers for a processor\n");
    DBGPrint("rc                       - display control registers \n");
    DBGPrint("rs                       - display segment registers \n");
    DBGPrint("rg                       - display general registers \n");
    DBGPrint("ra                       - display all registers\n");
    DBGPrint("rn                       - display coprocessor/MMX registers\n");

    return 1;
}

/* RC */

unsigned long displayControlRegisters(unsigned char *cmd,
			     StackFrame *stackFrame, unsigned long Exception,
			     DEBUGGER_PARSER *parser)
{
     DBGPrint("Control Registers\n");
     DisplayControlRegisters(get_processor_id(), stackFrame);
     return 1;

}

/* RA */

unsigned long displayAllRegisters(unsigned char *cmd,
			     StackFrame *stackFrame, unsigned long Exception,
			     DEBUGGER_PARSER *parser)
{
     register unsigned long processor = get_processor_id();

     DBGPrint("General Registers\n");
     DisplayGeneralRegisters(stackFrame);

     DBGPrint("Segment Registers\n");
     DisplaySegmentRegisters(stackFrame);

     DBGPrint("Control Registers\n");
     DisplayControlRegisters(processor, stackFrame);

     if (fpu_present())
     {
	DBGPrint("Coprocessor Registers\n");
	DisplayNPXRegisters(processor);
     }
     else
     {
	DBGPrint("Coprocessor Not Present\n");
     }
     return 1;

}


/* RS */

unsigned long displaySegmentRegisters(unsigned char *cmd,
			     StackFrame *stackFrame, unsigned long Exception,
			     DEBUGGER_PARSER *parser)
{
     DBGPrint("Segment Registers\n");
     DisplaySegmentRegisters(stackFrame);
     return 1;

}

/* RN */

unsigned long displayNumericRegisters(unsigned char *cmd,
			     StackFrame *stackFrame, unsigned long Exception,
			     DEBUGGER_PARSER *parser)
{
     if (fpu_present())
     {
	DBGPrint("Coprocessor Registers\n");
	DisplayNPXRegisters(get_processor_id());
     }
     else
     {
	DBGPrint("Coprocessor Not Present\n");
     }
     return 1;

}

/* RG */

unsigned long displayGeneralRegisters(unsigned char *cmd,
			     StackFrame *stackFrame, unsigned long Exception,
			     DEBUGGER_PARSER *parser)
{
     DBGPrint("General Registers\n");
     DisplayGeneralRegisters(stackFrame);
     return 1;
}

#ifdef RENDER_NPX_VALUES
double ldexp(double v, int e)
{
   double two = 2.0;

   if (e < 0)
   {
      e = -e; /* This just might overflow on two-complement machines. */
      if (e < 0)
         return 0.0;

      while (e > 0)
      {
	 if (e & 1)
         {
            v = v / two;
         }
	 two = two * two;
	 e >>= 1;
      }
   }
   else
   if (e > 0)
   {
      while (e > 0)
      {
	 if (e & 1)
            v = v * two;
	 two = two * two;
	 e >>= 1;
      }
   }
   return v;
}
#endif

void DisplayNPXRegisters(unsigned long processor)
{
     register int i;
     int tag;
     int tos;
#ifdef RENDER_NPX_VALUES
     double d;
#endif

     tos = (npx[processor].status >> 11) & 7;
     if (tos) {};

     DBGPrint("Control: 0x%04X  Status: 0x%04X  Tag: 0x%04X  TOS: %i CPU: %i\n",
	       (unsigned)npx[processor].control & 0xFFFF,
               (unsigned)npx[processor].status & 0xFFFF,
               (unsigned)npx[processor].tag & 0xFFFF,
               (int)tos, (int)processor);

     for (i = 0; i < 8; i++)
     {
	tos = (npx[processor].status >> 11) & 7;
	DBGPrint("st(%d)/MMX%d  ", i, (int)((tos + i) % 8));

	if (npx[processor].reg[i].sign)
	    DBGPrint("-");
	else
	    DBGPrint("+");

	DBGPrint(" %04X %04X %04X %04X e %04X    ",
		 (unsigned)npx[processor].reg[i].sig3,
		 (unsigned)npx[processor].reg[i].sig2,
		 (unsigned)npx[processor].reg[i].sig1,
		 (unsigned)npx[processor].reg[i].sig0,
		 (unsigned)npx[processor].reg[i].exponent);

	 if (tos) {};
	 tag = (npx[processor].tag >> (((i + tos) % 8) * 2)) & 3;
	 switch (tag)
	 {

	    case 0:
	       DBGPrint("Valid");
#ifdef RENDER_NPX_VALUES
	       if (((int) npx[processor].reg[i].exponent - 16382 < 1000) &&
		  ((int) npx[processor].reg[i].exponent - 16382 > -1000))
	       {
		  d =
                  npx[processor].reg[i].sig3 / 65536.0 +
                  npx[processor].reg[i].sig2 / 65536.0 / 65536.0 +
                  npx[processor].reg[i].sig1 / 65536.0 / 65536.0 / 65536.0;

		  d = ldexp(d, (int) npx[processor].reg[i].exponent - 16382);

	          if (npx[processor].reg[i].sign)
		     d = -d;

		  DBGPrint("  %.16g", d);
	       }
	       else
		  DBGPrint("  (too big to display)");
#endif
	       DBGPrint("\n");
	       break;

	    case 1:
	       DBGPrint("Zero\n");
	       break;

	    case 2:
	       DBGPrint("Special\n");
	       break;

	    case 3:
	       DBGPrint("Empty\n");
	       break;
	}
     }
}

/* R */

unsigned long displayDefaultRegisters(unsigned char *cmd,
			     StackFrame *stackFrame, unsigned long Exception,
			     DEBUGGER_PARSER *parser)
{
    register unsigned long processor = get_processor_id();

    DisplayGeneralRegisters(stackFrame);

    if (control_toggle)
       DisplayControlRegisters(processor, stackFrame);

    if (numeric_toggle)
       DisplayNPXRegisters(processor);

     disassemble(stackFrame, stackFrame->tIP, 1, -1, 0);
     return 1;

}

void displayRegisters(StackFrame *stackFrame, unsigned long processor)
{
    if (general_toggle)
       DisplayGeneralRegisters(stackFrame);

    if (control_toggle)
       DisplayControlRegisters(processor, stackFrame);

    if (numeric_toggle)
       DisplayNPXRegisters(processor);

}

//
// IA32 Registers
//

unsigned long displayEAXHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* EAX */

unsigned long ChangeEAXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tAX = value;
	DBGPrint("EAX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


/* ORIGEAX */

unsigned long ChangeORIGEAXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tReserved[1] = value;
	DBGPrint("ORIGEAX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayEBXHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* EBX */

unsigned long ChangeEBXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tBX = value;
	DBGPrint("EBX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayECXHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* ECX */

unsigned long ChangeECXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tCX = value;
	DBGPrint("ECX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayEDXHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* EDX */

unsigned long ChangeEDXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tDX = value;
	DBGPrint("EDX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayESIHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* ESI */

unsigned long ChangeESIRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tSI = value;
	DBGPrint("ESI changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayEDIHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* EDI */

unsigned long ChangeEDIRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tDI = value;
	DBGPrint("EDI changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayEBPHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* EBP */

unsigned long ChangeEBPRegister(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tBP = value;
	DBGPrint("EBP changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayESPHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* ESP */

unsigned long ChangeESPRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tSP = value;
	DBGPrint("ESP changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayEIPHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* EIP */

unsigned long ChangeEIPRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tIP = value;
	DBGPrint("EIP changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

#ifdef CONFIG_X86_64
//
// X86_64 Registers
//

unsigned long displayRAXHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RAX */

unsigned long ChangeRAXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tAX = value;
	DBGPrint("RAX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

/* ORIGRAX */

unsigned long ChangeORIGRAXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tReserved[1] = value;
	DBGPrint("ORIGRAX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayRBXHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RBX */

unsigned long ChangeRBXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tBX = value;
	DBGPrint("RBX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayRCXHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RCX */

unsigned long ChangeRCXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tCX = value;
	DBGPrint("RCX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayRDXHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RDX */

unsigned long ChangeRDXRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tDX = value;
	DBGPrint("RDX changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayRSIHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RSI */

unsigned long ChangeRSIRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tSI = value;
	DBGPrint("RSI changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayRDIHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RDI */

unsigned long ChangeRDIRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tDI = value;
	DBGPrint("RDI changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayRBPHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RBP */

unsigned long ChangeRBPRegister(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tBP = value;
	DBGPrint("RBP changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayRSPHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RSP */

unsigned long ChangeRSPRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tSP = value;
	DBGPrint("RSP changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}


unsigned long displayRIPHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RIP */

unsigned long ChangeRIPRegister(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->tIP = value;
	DBGPrint("RIP changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayR8Help(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* R8 */

unsigned long ChangeR8Register(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->r8 = value;
	DBGPrint("R8 changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayR9Help(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* R9 */

unsigned long ChangeR9Register(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->r9 = value;
	DBGPrint("R9 changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayR10Help(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* R10 */

unsigned long ChangeR10Register(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->r10 = value;
	DBGPrint("R10 changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayR11Help(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* R11 */

unsigned long ChangeR11Register(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->r11 = value;
	DBGPrint("R11 changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayR12Help(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* R12 */

unsigned long ChangeR12Register(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->r12 = value;
	DBGPrint("R12 changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayR13Help(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* R13 */

unsigned long ChangeR13Register(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->r13 = value;
	DBGPrint("R13 changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayR14Help(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* R14 */

unsigned long ChangeR14Register(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->r14 = value;
	DBGPrint("R14 changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

unsigned long displayR15Help(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* R15 */

unsigned long ChangeR15Register(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	stackFrame->r15 = value;
	DBGPrint("R15 changed to 0x%X\n", (unsigned)value);
     }
     else
	DBGPrint("invalid change register command or address\n");
     return 1;

}

#endif

//
//  Segment Registers
//

unsigned long displayCSHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* CS */

unsigned long ChangeCSRegister(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;
     register unsigned short oldW;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldW = stackFrame->tCS;
	stackFrame->tCS = (unsigned short) value;
	DBGPrint("CS: = [%04X] changed to CS: = [%04X]\n",
			(unsigned)oldW, (unsigned) value);
     }
     else
	DBGPrint("invalid change segment register command or address\n");
     return 1;

}

unsigned long displayDSHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

#ifndef CONFIG_X86_64
/* DS */

unsigned long ChangeDSRegister(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;
     register unsigned short oldW;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldW = stackFrame->tDS;
	stackFrame->tDS = (unsigned short) value;
	DBGPrint("DS: = [%04X] changed to DS: = [%04X]\n",
			(unsigned)oldW, (unsigned)value);
     }
     else
	DBGPrint("invalid change segment register command or address\n");
     return 1;

}

unsigned long displayESHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* ES */

unsigned long ChangeESRegister(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;
     register unsigned short oldW;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldW = stackFrame->tES;
	stackFrame->tES = (unsigned short) value;
	DBGPrint("ES: = [%04X] changed to ES: = [%04X]\n",
			(unsigned)oldW, (unsigned) value);
     }
     else
	DBGPrint("invalid change segment register command or address\n");
     return 1;

}
#endif

unsigned long displayFSHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* FS */

unsigned long ChangeFSRegister(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;
     register unsigned short oldW;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldW = stackFrame->tFS;
	stackFrame->tFS = (unsigned short) value;
	DBGPrint("FS: = [%04X] changed to FS: = [%04X]\n",
			(unsigned)oldW, (unsigned)value);
     }
     else
	DBGPrint("invalid change segment register command or address\n");
     return 1;

}

unsigned long displayGSHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* GS */

unsigned long ChangeGSRegister(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;
     register unsigned short oldW;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldW = stackFrame->tGS;
	stackFrame->tGS = (unsigned short) value;
	DBGPrint("GS: = [%04X] changed to GS: = [%04X]\n",
			(unsigned)oldW, (unsigned)value);
     }
     else
	DBGPrint("invalid change segment register command or address\n");
     return 1;

}

unsigned long displaySSHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* SS */

unsigned long ChangeSSRegister(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value;
     register unsigned short oldW;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldW = stackFrame->tSS;
	stackFrame->tSS = (unsigned short) value;
	DBGPrint("SS: = [%04X] changed to SS: = [%04X]\n",
			(unsigned)oldW, (unsigned)value);
     }
     else
	DBGPrint("invalid change segment register command or address\n");

     return 1;

}

unsigned long displayRFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* RF */

unsigned long ChangeRFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & RF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= RF_FLAG) : (stackFrame->tSystemFlags &= ~RF_FLAG);
	DBGPrint("EFlag RF[%X] changed to (%d)\n",
			(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayTFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* TF */

unsigned long ChangeTFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & TF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= TF_FLAG) : (stackFrame->tSystemFlags &= ~TF_FLAG);
	DBGPrint("EFlag TF[%X] changed to (%d)\n",
			(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayZFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* ZF */

unsigned long ChangeZFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & ZF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= ZF_FLAG) : (stackFrame->tSystemFlags &= ~ZF_FLAG);
	DBGPrint("EFlag ZF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displaySFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* SF */

unsigned long ChangeSFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & SF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= SF_FLAG) : (stackFrame->tSystemFlags &= ~SF_FLAG);
	DBGPrint("EFlag SF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayPFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* PF */

unsigned long ChangePFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & PF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= PF_FLAG) : (stackFrame->tSystemFlags &= ~PF_FLAG);
	DBGPrint("EFlag PF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayCFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* CF */

unsigned long ChangeCFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & CF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= CF_FLAG) : (stackFrame->tSystemFlags &= ~CF_FLAG);
	DBGPrint("EFlag CF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayOFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* OF */

unsigned long ChangeOFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & OF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= OF_FLAG) : (stackFrame->tSystemFlags &= ~OF_FLAG);
	DBGPrint("EFlag OF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}


unsigned long displayIFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* IF */

unsigned long ChangeIFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & IF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= IF_FLAG) : (stackFrame->tSystemFlags &= ~IF_FLAG);
	DBGPrint("EFlag IF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayIDHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* ID */

unsigned long ChangeIDFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & ID_FLAGS;
	(value) ? (stackFrame->tSystemFlags |= ID_FLAGS) : (stackFrame->tSystemFlags &= ~ID_FLAGS);
	DBGPrint("EFlag ID[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayDFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* DF */

unsigned long ChangeDFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{

     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & DF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= DF_FLAG) : (stackFrame->tSystemFlags &= ~DF_FLAG);
	DBGPrint("EFlag DF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayNTHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* NT */

unsigned long ChangeNTFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & NT_FLAG;
	(value) ? (stackFrame->tSystemFlags |= NT_FLAG) : (stackFrame->tSystemFlags &= ~NT_FLAG);
	DBGPrint("EFlag NT[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayVMHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* VM */

unsigned long ChangeVMFlag(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & VM_FLAG;
	(value) ? (stackFrame->tSystemFlags |= VM_FLAG) : (stackFrame->tSystemFlags &= ~VM_FLAG);
	DBGPrint("EFlag VM[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}


unsigned long displayVIFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* VIF */

unsigned long ChangeVIFFlag(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & VIF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= VIF_FLAG) : (stackFrame->tSystemFlags &= ~VIF_FLAG);
	DBGPrint("EFlag VIF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayVIPHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* VIP */

unsigned long ChangeVIPFlag(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & VIP_FLAG;
	(value) ? (stackFrame->tSystemFlags |= VIP_FLAG) : (stackFrame->tSystemFlags &= ~VIP_FLAG);
	DBGPrint("EFlag VIP[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayAFHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* AF */

unsigned long ChangeAFFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & AF_FLAG;
	(value) ? (stackFrame->tSystemFlags |= AF_FLAG) : (stackFrame->tSystemFlags &= ~AF_FLAG);
	DBGPrint("EFlag AF[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}


unsigned long displayACHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    return 1;
}

/* AC */

unsigned long ChangeACFlag(unsigned char *cmd,
		  StackFrame *stackFrame, unsigned long Exception,
		  DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long value, oldD;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	oldD = stackFrame->tSystemFlags & AC_FLAG;
	(value) ? (stackFrame->tSystemFlags |= AC_FLAG) : (stackFrame->tSystemFlags &= ~AC_FLAG);
	DBGPrint("EFlag AC[%X] changed to (%d)\n",
				(unsigned)oldD, (int)value);
     }
     else
	DBGPrint("invalid change flags command\n");
     return 1;

}

unsigned long displayMTRRHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("mtrr                     - display memory type range registers\n");
    return 1;
}

/* MTRR */

unsigned long DisplayMTRRRegisters(unsigned char *cmd,
			  StackFrame *stackFrame, unsigned long Exception,
			  DEBUGGER_PARSER *parser)
{
     displayMTRRRegisters();
     return 1;

}

unsigned long displayGDTHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint(".g or .g <address>       - display global descriptor table\n");
    return 1;
}

/* .G */

unsigned long displayGDT(unsigned char *cmd,
		StackFrame *stackFrame, unsigned long Exception,
		DEBUGGER_PARSER *parser)
{
     register unsigned long address;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
	DisplayGDT((unsigned char *) address);
     else
	DisplayGDT((unsigned char *) 0);
     return 1;
}

unsigned long displayIDTHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint(".i or .i <address>       - display interrupt descriptor table\n");
    return 1;
}

/* .I */

unsigned long displayIDT(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
	DisplayIDT((unsigned char *) address);
     else
	DisplayIDT((unsigned char *) 0);
     return 1;
}

unsigned long evaluateExpressionHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    extern void displayExpressionHelp(void);

    displayExpressionHelp();
    return 1;
}

/* .E */

unsigned long evaluateExpression(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     extern void EvaluateCommandExpression(StackFrame *stackFrame,
                                           unsigned char *cmd);

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     EvaluateCommandExpression(stackFrame, cmd);
     return 1;
}

unsigned long portCommandHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("i   <port>               - input byte from port\n");
    DBGPrint("ib  <port>               - input byte from port\n");
    DBGPrint("iw  <port>               - input word from port\n");
    DBGPrint("il  <port>               - input double word from port\n");
    DBGPrint("o   <port> <val>         - output byte to port\n");
    DBGPrint("ob  <port> <val>         - output byte to port\n");
    DBGPrint("ow  <port> <val>         - output word to port\n");
    DBGPrint("ol  <port> <val>         - output double word to port\n");
    return 1;
}

/* IW */

unsigned long inputWordPort(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	DBGPrint("inportw (%04X) = %04X\n",
                     (unsigned)address, (unsigned)inw(address));
     }
     else
     {
	DBGPrint("bad port command\n");
     }
     return 1;

}

/* ID */

unsigned long inputDoublePort(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	DBGPrint("inportd (%04X) = %X\n",
			  (unsigned)address, (unsigned)inl(address));
     }
     else
     {
	DBGPrint("bad port command\n");
     }
     return 1;

}

/* IB */

unsigned long inputBytePort(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	DBGPrint("inportb (%04X) = %02X\n",
			  (unsigned)address, (unsigned)inb(address));
     }
     else
     {
	DBGPrint("bad port command\n");
     }
     return 1;

}

/* I */

unsigned long inputPort(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     register unsigned long address;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	DBGPrint("inportb (%04X) = %02X\n",
			  (unsigned)address, (unsigned)inb(address));
     }
     else
     {
	DBGPrint("bad port command\n");
     }
     return 1;

}

/* OW */

unsigned long outputWordPort(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long port, value;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     port = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	value = EvaluateExpression(stackFrame, &cmd, &valid);
	if (valid)
	{
	   DBGPrint("outportw (%04X) = %04X\n",
				    (unsigned)port, (unsigned)value);
	   outw(port, value);
	   return 1;
	}
     }
     else
	DBGPrint("bad port command\n");

     return 1;

}

/* OD */

unsigned long outputDoublePort(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long port, value;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     port = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	value = EvaluateExpression(stackFrame, &cmd, &valid);
	if (valid)
	{
	   DBGPrint("outportd (%04X) = %X\n",
			    (unsigned)port, (unsigned)value);
	   outl(port, value);
	   return 1;
	}
     }
     else
	DBGPrint("bad port command\n");

     return 1;

}

/* OB */

unsigned long outputBytePort(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long port, value;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     port = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	value = EvaluateExpression(stackFrame, &cmd, &valid);
	if (valid)
	{
	   DBGPrint("outportb (%04X) = %02X\n",
			    (unsigned)port, (unsigned)value);
	   outb(port, value);
	   return 1;
	}
     }
     else
	DBGPrint("bad port command\n");

     return 1;

}

/* O */

unsigned long outputPort(unsigned char *cmd,
		StackFrame *stackFrame, unsigned long Exception,
		DEBUGGER_PARSER *parser)
{
     register unsigned long port, value;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     port = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	value = EvaluateExpression(stackFrame, &cmd, &valid);
	if (valid)
	{
	   DBGPrint("outportb (%04X) = %02X\n",
			    (unsigned)port, (unsigned)value);
	   outb(port, value);
	   return 1;
	}
     }
     else
	DBGPrint("bad port command\n");

     return 1;

}

unsigned long breakpointCommandHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("b                        - display all breakpoints\n");
    DBGPrint("b   <address>            - set execute breakpoint\n");
    DBGPrint("bc  [#] (1-4)            - clear breakpoint\n");
    DBGPrint("bca                      - clear all breakpoints\n");
    DBGPrint("br[#] <address>          - set read/write breakpoint #=1,2 or 4 byte len\n");
    DBGPrint("bw[#] <address>          - set write only breakpoint #=1,2 or 4 byte len\n");
    DBGPrint("bi[#] <address>          - set io address breakpoint #=1,2 or 4 byte len\n");
    DBGPrint("bm  [p#]                 - mask breaks for specific processor \n");
    DBGPrint("bst                      - display temporary (go/proceed) breakpoints\n");
    return 1;
}

/* BCA */

unsigned long breakpointClearAll(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long i;

     for (i = 0; i < 4; i++)
     {
	BreakReserved[i] = 0;
	BreakPoints[i] = 0;
	BreakType[i] = 0;
	BreakLength[i] = 0;
	ConditionalBreakpoint[i] = 0;
     }
     SetDebugRegisters();
     DBGPrint("all breakpoints cleared\n");

     return 1;

}

/* BC */

unsigned long breakpointClear(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     register unsigned long i, address;
     register unsigned char *symbolName;
     register unsigned char *moduleName;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	i = address;
	if (i < 4)
	{
	   symbolName = GetSymbolFromValue(BreakPoints[i], &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	   moduleName = GetModuleInfoFromSymbolValue(BreakPoints[i],
                                           &modbuf[c][0], MAX_SYMBOL_LEN);
           if (moduleName)
	      DBGPrint("breakpoint %i at 0x%X (%s %s) %s|%s cleared\n",
			     (int)i,
			     (unsigned)BreakPoints[i],
			     BreakDescription[(BreakType[i] & 3)],
			     BreakLengthDescription[(BreakLength[i] & 3)],
			     ((char *)(moduleName) ? (char *)(moduleName)
                             : (char *)("")),
			     ((char *)(symbolName) ? (char *)(symbolName)
                             : (char *)("")));
           else
	      DBGPrint("breakpoint %i at 0x%X (%s %s) %s cleared\n",
			     (int)i,
			     (unsigned)BreakPoints[i],
			     BreakDescription[(BreakType[i] & 3)],
			     BreakLengthDescription[(BreakLength[i] & 3)],
			     ((char *)(symbolName) ? (char *)(symbolName)
                             : (char *)("")));
	   BreakReserved[i] = 0;
	   BreakPoints[i] = 0;
	   BreakType[i] = 0;
	   BreakLength[i] = 0;
	   ConditionalBreakpoint[i] = 0;
	   SetDebugRegisters();
	   return 1;
	}
	else
	   DBGPrint("breakpoint out of range\n");
	return 1;
     }
     DBGPrint("breakpoint not found\n");
     return 1;
}

/* BM */

unsigned long breakpointMask(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, pnum, i;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	pnum = address;
	if (pnum < MAX_PROCESSORS)
	{
	   if (BreakMask[pnum])
	      BreakMask[pnum] = 0;
	   else
	      BreakMask[pnum] = 1;
	   DBGPrint("processor %i : %s\n", (int)pnum,
		   BreakMask[pnum] ? "BREAKS_MASKED" : "BREAKS_UNMASKED");
	}
	else
	   DBGPrint("processor (%i) invalid\n", (int)pnum);
     }
     else
     {
	for (i = 0; i < MAX_PROCESSORS; i++)
	{
	   DBGPrint("processor %i : %s\n", (int)i,
		    BreakMask[i] ? "BREAKS_MASKED" : "BREAKS_UNMASKED");
	}
     }
     return 1;

}

/* BW1 */

unsigned long breakpointWord1(unsigned char *cmd,
		    StackFrame *stackFrame, unsigned long Exception,
		    DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_WRITE;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
		               ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}

/* BW2 */

unsigned long breakpointWord2(unsigned char *cmd,
		    StackFrame *stackFrame, unsigned long Exception,
		    DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();


     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_WRITE;
	      BreakLength[i] = TWO_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}

/* BW4 */

unsigned long breakpointWord4(unsigned char *cmd,
		    StackFrame *stackFrame, unsigned long Exception,
		    DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_WRITE;
	      BreakLength[i] = FOUR_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}

/* BW */

unsigned long breakpointWord(unsigned char *cmd,
		    StackFrame *stackFrame, unsigned long Exception,
		    DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_WRITE;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}


/* BR1 */

unsigned long breakpointRead1(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_READWRITE;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}


/* BR2 */

unsigned long breakpointRead2(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_READWRITE;
	      BreakLength[i] = TWO_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}


/* BR4 */

unsigned long breakpointRead4(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_READWRITE;
	      BreakLength[i] = FOUR_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}


/* BR */

unsigned long breakpointRead(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_READWRITE;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}

/* BI1 */

unsigned long breakpointIO1(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_IOPORT;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}

/* BI2 */

unsigned long breakpointIO2(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_IOPORT;
	      BreakLength[i] = TWO_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}

/* BI4 */

unsigned long breakpointIO4(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_IOPORT;
	      BreakLength[i] = FOUR_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}

/* BI */

unsigned long breakpointIO(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; (r < 255) && (*pB); r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_IOPORT;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     else
	DBGPrint("breakpoint parameters invalid\n");
     return 1;

}

/* B */

unsigned long breakpointExecute(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     register unsigned long address, i, r;
     register unsigned char *pB, *symbolName;
     register unsigned char *moduleName;
     unsigned long valid;
     register int c = get_processor_id();

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
     {
        register int found = 0;

	for (i = 0; i < 4; i++)
	{
	   if (BreakReserved[i])
	   {
	      symbolName = GetSymbolFromValue(BreakPoints[i], &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(BreakPoints[i],
                                           &modbuf[c][0], MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("Break %i is at 0x%X (%s %s) %s|%s\n",
				(int)i,
				(unsigned)BreakPoints[i],
				BreakDescription[(BreakType[i] & 3)],
				BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                : (char *)("")),
			        ((char *)(symbolName) ? (char *)(symbolName)
                                : (char *)("")));
              else
	         DBGPrint("Break %i is at 0x%X (%s %s) %s\n",
				(int)i,
				(unsigned)BreakPoints[i],
				BreakDescription[(BreakType[i] & 3)],
				BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(symbolName) ? (char *)(symbolName)
                                : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);
              found = 1;
	   }
	}
        if (!found)
           DBGPrint("no breakpoints currently defined\n");

     }
     else
     {
	for (i = 0; i < 4; i++)
	{
	   if (!BreakReserved[i])
	   {
	      pB = cmd;
	      EvaluateExpression(stackFrame, &cmd, &valid);
	      if (valid)
	      {
		 ConditionalBreakpoint[i] = 1;
		 for (r = 0; r < 255 && *pB; r++)
		    BreakCondition[i][r] = *pB++;
		 BreakCondition[i][r] = '\0';
	      }
	      BreakReserved[i] = 1;
	      BreakPoints[i] = address;
	      BreakType[i] = BREAK_EXECUTE;
	      BreakLength[i] = ONE_BYTE_FIELD;
	      symbolName = GetSymbolFromValue(address, &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	      moduleName = GetModuleInfoFromSymbolValue(address, &modbuf[c][0],
                                           MAX_SYMBOL_LEN);
              if (moduleName)
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s|%s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(moduleName) ? (char *)(moduleName)
                                 : (char *)("")),
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));
              else
	         DBGPrint("breakpoint %i set to 0x%X (%s %s) %s\n",
				 (int)i,
				 (unsigned)BreakPoints[i],
				 BreakDescription[(BreakType[i] & 3)],
				 BreakLengthDescription[(BreakLength[i] & 3)],
			         ((char *)(symbolName) ? (char *)(symbolName)
                                 : (char *)("")));

	      if (ConditionalBreakpoint[i])
		 DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

	      SetDebugRegisters();
	      return 1;
	   }
	}
	DBGPrint("no breakpoint available\n");
     }
     return 1;

}

/* BST */

unsigned long breakpointShowTemp(unsigned char *cmd,
			StackFrame *stackFrame, unsigned long Exception,
			DEBUGGER_PARSER *parser)
{
     register unsigned long i;
     register unsigned char *symbolName;
     register unsigned char *moduleName;
     register int found = 0;
     register int c = get_processor_id();

     for (i = 0; i < 4; i++)
     {
	if (BreakReserved[i] && BreakTemp[i])
	{
	   symbolName = GetSymbolFromValue(BreakPoints[i], &symbuf[c][0],
                                           MAX_SYMBOL_LEN);
	   moduleName = GetModuleInfoFromSymbolValue(BreakPoints[i],
                                           &modbuf[c][0], MAX_SYMBOL_LEN);
           if (moduleName)
	      DBGPrint("Break %i is at 0x%X (%s %s) %s|%s [%s]\n",
				(int)i,
				(unsigned)BreakPoints[i],
				BreakDescription[(BreakType[i] & 3)],
				BreakLengthDescription[(BreakLength[i] & 3)],
			       ((char *)(moduleName) ? (char *)(moduleName)
                                : (char *)("")),
			        ((char *)(symbolName) ? (char *)(symbolName)
                                : (char *)("")),
				BreakGo[i] ? "GO" : BreakProceed[i]
                                ? "PROCEED" : "");
           else
	      DBGPrint("Break %i is at 0x%X (%s %s) %s [%s]\n",
				(int)i,
				(unsigned)BreakPoints[i],
				BreakDescription[(BreakType[i] & 3)],
				BreakLengthDescription[(BreakLength[i] & 3)],
			        ((char *)(symbolName) ? (char *)(symbolName)
                                : (char *)("")),
				BreakGo[i] ? "GO" : BreakProceed[i]
                                ? "PROCEED" : "");
	   if (ConditionalBreakpoint[i])
	      DBGPrint("if (%s) is TRUE\n", BreakCondition[i]);

           found = 1;
	}
     }
     if (!found)
        DBGPrint("no temporary breakpoints defined\n");

     return 1;

}

unsigned long displayProcessorStatusHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("displays active processors and their current state\n");
    return 1;
}

unsigned long displayProcessorStatus(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     register unsigned long i;

     for (i = 0; i < MAX_PROCESSORS; i++)
     {
        if (cpu_online(i))
        {
	   DBGPrint("Processor: (%i)  State:  %s\n",
	              i, procState[ProcessorState[i] & 0xF]);
        }
     }
     return 1;
}

void displayMTRRRegisters(void)
{
    register int i;
    unsigned long base1, base2;
    unsigned long mask1, mask2;
    extern unsigned long cpu_mttr_on(void);

    if (cpu_mttr_on())
    {
       DBGPrint("memory type range registers\n");
       for (i = 0; i < 8; i++)
       {
	  ReadMSR(MTRR_BASE_REGS[i], &base1, &base2);
	  ReadMSR(MTRR_MASK_VALUES[i], &mask1, &mask2);
	  DBGPrint("MTRR_BASE_%i  %X:%X   MTRR_MASK_%i  %X:%X\n",
			   (int)i,
                           (unsigned)base1, (unsigned)base2,
                           (int)i,
                           (unsigned)mask1, (unsigned)mask2);
       }
    }
    else
       DBGPrint("memory type range registers are Pentium Pro/II/Xeon and above\n");
    return;
}

void DisplayGDT(unsigned char *GDT_ADDRESS)
{

    register int i, r;
    unsigned long count;
    unsigned long gdt_pointer;
    unsigned short gdt_index;
    unsigned char *p;
    unsigned char GDTR[16];
    GDT *gdt;
#ifndef CONFIG_X86_64
    TSS *tss;
#endif
    union
    {
       GDT lgdt;
       unsigned char data[16];
    } lg;


#ifdef CONFIG_X86_64
    ReadGDTR((unsigned long *)&GDTR[0]);
    gdt_index = mdb_getword((unsigned long)&GDTR[0], 2);
    gdt_pointer = mdb_getword((unsigned long)&GDTR[2], 8);

    DBGPrint("GDTR: %04X:%llX  Processor: %i\n",
                  (unsigned)gdt_index, (unsigned long long)gdt_pointer,
                  (int)get_processor_id());

    count = 0;
    gdt_index = (gdt_index + 7) / 8;
    p = (unsigned char *) gdt_pointer;
    for (i = 0; i < gdt_index; i++)
    {
       if (DBGPrint("%04X(%04i):", (unsigned) count, (int)i)) return;
       for (r = 0; r < 16; r++)
	  lg.data[r] = (unsigned char) mdb_getword((unsigned long)&p[r], 1);

       gdt = (GDT *) &lg.lgdt;
       if (DBGPrint(
              "0x%04lX%04X%08X seg:%04X dpl:%02X type:%02X p:%02X i:%02X", 
	            gdt->offset_low,
    	            gdt->offset_middle,
                    gdt->offset_high,
	            gdt->segment,
                    gdt->dpl, 
                    gdt->type, 
                    gdt->p,
                    gdt->ist)) return;
       if (DBGPrint("\n")) return;

       p = (void *)((unsigned long) p + (unsigned long) 8);
       count += 16;
    }

#else
    ReadGDTR((unsigned long *)&GDTR[0]);
    gdt_index = mdb_getword((unsigned long)&GDTR[0], 2);
    gdt_pointer = mdb_getword((unsigned long)&GDTR[2], 4);

    DBGPrint("GDTR: %04X:%X  Processor: %i\n",
                  (unsigned)gdt_index, (unsigned)gdt_pointer,
                  (int)get_processor_id());

    count = 0;
    gdt_index = (gdt_index + 7) / 8;
    p = (unsigned char *) gdt_pointer;
    for (i = 0; i < gdt_index; i++)
    {
       if (DBGPrint("%X (%04i):", (unsigned) count, (int)i)) return;
       for (r = 0; r < 8; r++)
       {
	  lg.data[r] = (unsigned char) mdb_getword((unsigned long)&p[r], 1);
	  if (DBGPrint(" %02X", (unsigned char) lg.data[r])) return;
       }

       gdt = (GDT *) &lg.lgdt;
       if ((gdt->GDTType & 0x92) == 0x92)
       {
	  if (DBGPrint("  b:%X lim:%X t:%02X ot:%02X",
		   ((gdt->Base3 << 24) | (gdt->Base2 << 16) |
                   (gdt->Base1)),
		   (((gdt->OtherType & 0xF) << 16) | (gdt->Limit)),
		   gdt->GDTType, gdt->OtherType)) return;
       }
       else if ((gdt->GDTType & 0x89) == 0x89)
       {
	  tss = (TSS *) gdt;
	  if (DBGPrint("  tss:%X lim:%04X t:%02X ot:%02X",
		      ((tss->TSSBase3 << 24) | (tss->TSSBase2 << 16) |
                      (tss->TSSBase1)),
		      tss->TSSLimit, tss->TSSType,
                      tss->TSSOtherType)) return;
       }
       if (DBGPrint("\n")) return;

       p = (void *)((unsigned long) p + (unsigned long) 8);
       count += 8;
    }
#endif
    return;

}

void DisplayIDT(unsigned char *IDT_ADDRESS)
{

    register int i, r;
    unsigned long count;
    unsigned long idt_pointer;
    unsigned short idt_index;
    unsigned char *p;
    unsigned char IDTR[16];
    IDT *idt;
#ifndef CONFIG_X86_64
    TSS_GATE *tss_gate;
#endif
    union
    {
       IDT lidt;
       unsigned char data[16];
    } id;

#ifdef CONFIG_X86_64
    ReadIDTR((unsigned long *)&IDTR[0]);
    idt_index = mdb_getword((unsigned long)&IDTR[0], 2);
    idt_pointer = mdb_getword((unsigned long)&IDTR[2], 8);

    DBGPrint("IDTR: %04X:%llX  Processor: %i\n",
                    (unsigned)idt_index, (unsigned long long)idt_pointer,
                    (int)get_processor_id());

    count = 0;
    idt_index = (idt_index + 7) / 8;
    p = (unsigned char *) idt_pointer;
    for (i = 0; i < idt_index; i++)
    {
       if (DBGPrint("%04X(%04i):", (unsigned)count, (int)i)) return;
       for (r = 0; r < 16; r++)
	   id.data[r] = mdb_getword((unsigned long)&p[r], 1);

       idt = (IDT *) &id.lidt;
       if (DBGPrint(
              "0x%04lX%04X%08X seg:%04X dpl:%02X type:%02X p:%02X i:%02X", 
	            idt->offset_low,
    	            idt->offset_middle,
                    idt->offset_high,
	            idt->segment,
                    idt->dpl, 
                    idt->type, 
                    idt->p,
                    idt->ist)) return;

       if (DBGPrint("\n")) return;

       p = (void *)((unsigned long) p + (unsigned long) 8);
       count += 16;
    }
#else
    ReadIDTR((unsigned long *)&IDTR[0]);
    idt_index = mdb_getword((unsigned long)&IDTR[0], 2);
    idt_pointer = mdb_getword((unsigned long)&IDTR[2], 4);

    DBGPrint("IDTR: %04X:%X  Processor: %i\n",
                    (unsigned)idt_index, (unsigned)idt_pointer,
                    (int)get_processor_id());

    count = 0;
    idt_index = (idt_index + 7) / 8;
    p = (unsigned char *) idt_pointer;
    for (i = 0; i < idt_index; i++)
    {
       if (DBGPrint("%X (%04i):", (unsigned)count, (int)i)) return;
       for (r = 0; r < 8; r++)
       {
	   id.data[r] = mdb_getword((unsigned long)&p[r], 1);
	   if (DBGPrint(" %02X", (unsigned char) id.data[r])) return;
       }
       idt = (IDT *) &id.lidt;
       if ((idt->IDTFlags & 0x8E) == 0x8E)
       {
	  if (DBGPrint("  b:%X s:%04X t:%02X ot:%02X",
			     ((idt->IDTHigh << 16) | (idt->IDTLow)),
			     idt->IDTSegment,
			     idt->IDTFlags, idt->IDTSkip)) return;

       }
       else if ((idt->IDTFlags & 0x85) == 0x85)
       {
	  tss_gate = (TSS_GATE *) idt;
	  if (DBGPrint("  task_gate: %04X t:%02X",
		     tss_gate->TSSSelector, tss_gate->TSSFlags)) return;
       }
       if (DBGPrint("\n")) return;

       p = (void *)((unsigned long) p + (unsigned long) 8);
       count += 8;
    }
#endif
    return;

}

void DisplayTSS(StackFrame *stackFrame)
{
#ifdef CONFIG_X86_64
    unsigned long i, f = 0;

    DBGPrint("Task State Segment at 0x%p\n", stackFrame);

    DBGPrint("RAX: %016lX  RBX: %016lX  RCX: %016lX\n",
       stackFrame->tAX, stackFrame->tBX, stackFrame->tCX);

    DBGPrint("RDX: %016lX  RSI: %016lX  RDI: %016lX\n",
       stackFrame->tDX, stackFrame->tSI, stackFrame->tDI);

    DBGPrint("RSP: %016lX  RBP: %016lX   R8: %016lX\n",
       stackFrame->tSP, stackFrame->tBP, stackFrame->r8);

    DBGPrint(" R9: %016lX  R10: %016lX  R11: %016lX\n", 
       stackFrame->r9, stackFrame->r10, stackFrame->r11);

    DBGPrint("R12: %016lX  R13: %016lX  R14: %016lX\n",
       stackFrame->r12, stackFrame->r13, stackFrame->r14);

    DBGPrint("R15: %016lX\n", stackFrame->r15);

    DBGPrint(
     "CS: %04X  DS: %04X  ES: %04X  FS: %04X  GS: %04X  SS: %04X  LDT: %04X\n",
       (unsigned)stackFrame->tCS, (unsigned)stackFrame->tDS,
       (unsigned)stackFrame->tES, (unsigned)stackFrame->tFS, 
       (unsigned)stackFrame->tGS, (unsigned)stackFrame->tSS,
       (unsigned)stackFrame->tLDT);

    DBGPrint("RIP: %016lX  FLAGS: %016lX ",
       stackFrame->tIP, stackFrame->tSystemFlags);

    DBGPrint(" (");
    for (i = 0; i < 22; i++)
    {
       if (IA32Flags[i])
       {
	  if ((stackFrame->tSystemFlags >> i) & 0x00000001)
	  {
	     if (f)
		DBGPrint(" ");
	     f = 1;
	     DBGPrint("%s", IA32Flags[i]);
	  }
       }
    }
    DBGPrint(")\n");

    DBGPrint("CR3: %016lX  IOMAP: %016lX  BLINK: %016lX\n",
        (unsigned)stackFrame->tCR3, (unsigned)stackFrame->tIOMap,
        (unsigned)stackFrame->tReserved[0]);

#else
    unsigned long i, f = 0;

    DBGPrint("Task State Segment at 0x%p\n", stackFrame);

    DBGPrint("EAX: %08X  EBX: %08X  ECX: %08X  EDX: %08X\n",
       (unsigned)stackFrame->tAX, (unsigned)stackFrame->tBX,
       (unsigned)stackFrame->tCX, (unsigned)stackFrame->tDX);

    DBGPrint("ESI: %08X  EDI: %08X  ESP: %08X  EBP: %08X\n",
       (unsigned)stackFrame->tSI, (unsigned)stackFrame->tDI,
       (unsigned)stackFrame->tSP, (unsigned)stackFrame->tBP);

    DBGPrint(
     "CS: %04X  DS: %04X  ES: %04X  FS: %04X  GS: %04X  SS: %04X  LDT: %04X\n",
       (unsigned)stackFrame->tCS, (unsigned)stackFrame->tDS,
       (unsigned)stackFrame->tES, (unsigned)stackFrame->tFS,
       (unsigned)stackFrame->tGS, (unsigned)stackFrame->tSS,
       (unsigned)stackFrame->tLDT);   

    DBGPrint("EIP: %08X  FLAGS: %08X ",
       (unsigned)stackFrame->tIP, (unsigned)stackFrame->tSystemFlags);

    DBGPrint(" (");
    for (i = 0; i < 22; i++)
    {
       if (IA32Flags[i])
       {
	  if ((stackFrame->tSystemFlags >> i) & 0x00000001)
	  {
	     if (f)
		DBGPrint(" ");
	     f = 1;
	     DBGPrint("%s", IA32Flags[i]);
	  }
       }
    }
    DBGPrint(")\n");

    DBGPrint("CR3: %08X  IOMAP: %08X  BLINK: %08X\n",
        (unsigned)stackFrame->tCR3, (unsigned)stackFrame->tIOMap, 
        (unsigned)stackFrame->tReserved[0]);

#endif
}

void DisplayGeneralRegisters(StackFrame *stackFrame)
{
#ifdef CONFIG_X86_64
    unsigned long i, f = 0;

    DBGPrint("RAX: %016lX ", stackFrame->tAX);
    DBGPrint("RBX: %016lX ", stackFrame->tBX);
    DBGPrint("RCX: %016lX\n", stackFrame->tCX);
    DBGPrint("RDX: %016lX ", stackFrame->tDX);
    DBGPrint("RSI: %016lX ", stackFrame->tSI);
    DBGPrint("RDI: %016lX\n", stackFrame->tDI);
    DBGPrint("RSP: %016lX ", stackFrame->tSP);
    DBGPrint("RBP: %016lX ", stackFrame->tBP);
    DBGPrint(" R8: %016lX\n", stackFrame->r8);
    DBGPrint(" R9: %016lX ", stackFrame->r9);
    DBGPrint("R10: %016lX ", stackFrame->r10);
    DBGPrint("R11: %016lX\n", stackFrame->r11);
    DBGPrint("R12: %016lX ", stackFrame->r12);
    DBGPrint("R13: %016lX ", stackFrame->r13);
    DBGPrint("R14: %016lX\n", stackFrame->r14);
    DBGPrint("R15: %016lX ", stackFrame->r15);

    if (segment_toggle)
       DisplaySegmentRegisters(stackFrame);
    else
       DBGPrint("\n");

    DBGPrint(" IP: %016lX ", stackFrame->tIP);
    DBGPrint("FLAGS: %016lX ", stackFrame->tSystemFlags);

    DBGPrint(" (");
    for (i = 0; i < 22; i++)
    {
       if (IA32Flags[i])
       {
	  if ((stackFrame->tSystemFlags >> i) & 0x00000001)
	  {
	     if (f)
		DBGPrint(" ");
	     f = 1;
	     DBGPrint("%s", IA32Flags[i]);
	  }
       }
    }
    DBGPrint(")\n");

#else
    unsigned long i, f = 0;

    DBGPrint("EAX: %08X ", (unsigned)stackFrame->tAX);
    DBGPrint("EBX: %08X ", (unsigned)stackFrame->tBX);
    DBGPrint("ECX: %08X ", (unsigned)stackFrame->tCX);
    DBGPrint("EDX: %08X\n", (unsigned)stackFrame->tDX);
    DBGPrint("ESI: %08X ", (unsigned)stackFrame->tSI);
    DBGPrint("EDI: %08X ", (unsigned)stackFrame->tDI);
    DBGPrint("ESP: %08X ", (unsigned)stackFrame->tSP);
    DBGPrint("EBP: %08X\n", (unsigned)stackFrame->tBP);

    if (segment_toggle)
       DisplaySegmentRegisters(stackFrame);

    DBGPrint("EIP: %08X ", (unsigned)stackFrame->tIP);
    DBGPrint("EFLAGS: %08X ", (unsigned)stackFrame->tSystemFlags);

    DBGPrint(" (");
    for (i = 0; i < 22; i++)
    {
       if (IA32Flags[i])
       {
	  if ((stackFrame->tSystemFlags >> i) & 0x00000001)
	  {
	     if (f)
		DBGPrint(" ");
	     f = 1;
	     DBGPrint("%s", IA32Flags[i]);
	  }
       }
    }
    DBGPrint(")\n");
#endif
}

void DisplaySegmentRegisters(StackFrame *stackFrame)
{
#ifdef CONFIG_X86_64
    DBGPrint("CS: %04X ", (unsigned)stackFrame->tCS);
    DBGPrint("DS: %04X ", (unsigned)stackFrame->tDS);
    DBGPrint("ES: %04X ", (unsigned)stackFrame->tES);
    DBGPrint("FS: %04X ", (unsigned)stackFrame->tFS);
    DBGPrint("GS: %04X ", (unsigned)stackFrame->tGS);
    DBGPrint("SS: %04X\n", (unsigned)stackFrame->tSS);
#else
    DBGPrint("CS: %04X ", (unsigned)stackFrame->tCS);
    DBGPrint("DS: %04X ", (unsigned)stackFrame->tDS);
    DBGPrint("ES: %04X ", (unsigned)stackFrame->tES);
    DBGPrint("FS: %04X ", (unsigned)stackFrame->tFS);
    DBGPrint("GS: %04X ", (unsigned)stackFrame->tGS);
    DBGPrint("SS: %04X\n", (unsigned)stackFrame->tSS);
#endif
}

void DisplayControlRegisters(unsigned long processor, StackFrame *stackFrame)
{

    unsigned char GDTR[16], IDTR[16];

    if (stackFrame) {};

#ifdef CONFIG_X86_64
    DBGPrint("CR0: %016lX ", ReadCR0());
    DBGPrint("CR2: %016lX ", ReadCR2());
    DBGPrint("CR3: %016lX\n", ReadCR3());
    DBGPrint("CR4: %016lX ", ReadCR4());
    DBGPrint("DR0: %016lX ", ReadDR0());
    DBGPrint("DR1: %016lX\n", ReadDR1());
    DBGPrint("DR2: %016lX ", ReadDR2());
    DBGPrint("DR3: %016lX ", ReadDR3());
    DBGPrint("DR6: %016lX\n", ReadDR6());
    DBGPrint("DR7: %016lX ", ReadDR7());
    DBGPrint("VR6: %016lX ", CurrentDR6[processor]);
    DBGPrint("VR7: %016lX\n", CurrentDR7);

    ReadGDTR((unsigned long *)&GDTR[0]);
    ReadIDTR((unsigned long *)&IDTR[0]);

    DBGPrint("GDTR: %04X:%llX IDTR: %04X:%llX\n",
			(unsigned)*(unsigned short *)&GDTR[0],
                        *(unsigned long long *)&GDTR[2],
			(unsigned)*(unsigned short *)&IDTR[0],
                        *(unsigned long long *)&IDTR[2]);
    DBGPrint("LDTR: %04X  TR: %04X\n", 
                        (unsigned)ReadLDTR(),
                        (unsigned)ReadTR());
#else
    DBGPrint("CR0: %08X ", (unsigned)ReadCR0());
    DBGPrint("CR2: %08X ", (unsigned)ReadCR2());
    DBGPrint("CR3: %08X ", (unsigned)ReadCR3());
    DBGPrint("CR4: %08X\n", (unsigned)ReadCR4());
    DBGPrint("DR0: %08X ", (unsigned)ReadDR0());
    DBGPrint("DR1: %08X ", (unsigned)ReadDR1());
    DBGPrint("DR2: %08X ", (unsigned)ReadDR2());
    DBGPrint("DR3: %08X\n", (unsigned)ReadDR3());
    DBGPrint("DR6: %08X ", (unsigned)ReadDR6());
    DBGPrint("DR7: %08X ", (unsigned)ReadDR7());
    DBGPrint("VR6: %08X ", (unsigned)CurrentDR6[processor]);
    DBGPrint("VR7: %08X\n", (unsigned)CurrentDR7);

    DBGPrint("GDTR: %04X:%08X IDTR: %04X:%08X  LDTR: %04X  TR: %04X\n",
			(unsigned)*(unsigned short *)&GDTR[0],
                        (unsigned)*(unsigned long *)&GDTR[2],
			(unsigned)*(unsigned short *)&IDTR[0],
                        (unsigned)*(unsigned long *)&IDTR[2],
			(unsigned)ReadLDTR(),
                        (unsigned)ReadTR());
#endif

}

unsigned long ConsoleDisplayBreakReason(StackFrame *stackFrame,
                                        unsigned long Exception,
                                        unsigned long processor,
                                        unsigned long lastCommand)
{
       if (last_mdb_oops)
          DBGPrint("\nKernel Oops reported (%s)\n", last_mdb_oops);

       if ((CurrentDR6[processor] & B0_BIT) && (CurrentDR7 & G0_BIT) &&
            Exception == 1)
       {
	  if (BreakGo[0])
          {
	     DBGPrint("\nBreak at 0x%lX due to - GO breakpoint (0)\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if (BreakProceed[0])
          {
	     DBGPrint("\nBreak at 0x%lX due to - Proceed breakpoint (0)\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if (BreakPoints[0] && ConditionalBreakpoint[0])
	  {
	     DBGPrint("\nBreak at 0x%lX due to - breakpoint 0 (%s)\n",
				 stackFrame->tIP,
                                 BreakDescription[(BreakType[0] & 3)]);
	     DBGPrint("expr: %s was TRUE\n", BreakCondition[0]);
             return 1;
	  }
	  else
          if (BreakPoints[0])
          {
	     DBGPrint("\nBreak at 0x%lX due to - breakpoint 0 (%s)\n",
				 stackFrame->tIP,
                                 BreakDescription[(BreakType[0] & 3)]);
             return 1;
          }
          else
          {
	     DBGPrint("\nBreak at 0x%lX due to - INT1 breakpoint (B0)\n",
				 stackFrame->tIP);
             return 1;  /* not one of ours */
          }
       }
       else
       if ((CurrentDR6[processor] & B1_BIT) && (CurrentDR7 & G1_BIT) &&
            Exception == 1)
       {
	  if (BreakGo[1])
          {
	     DBGPrint("\nBreak at 0x%lX due to - GO breakpoint (1)\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if (BreakProceed[1])
          {
	     DBGPrint("\nBreak at 0x%lX due to - Proceed breakpoint (1)\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if (BreakPoints[1] && ConditionalBreakpoint[1])
	  {
	     DBGPrint("\nBreak at 0x%lX due to - breakpoint 1 (%s)\n",
				 stackFrame->tIP,
                                 BreakDescription[(BreakType[1] & 3)]);
	     DBGPrint("expr: %s was TRUE\n", BreakCondition[1]);
             return 1;
	  }
	  else
          if (BreakPoints[1])
          {
	     DBGPrint("\nBreak at 0x%lX due to - breakpoint 1 (%s)\n",
				 stackFrame->tIP,
                                 BreakDescription[(BreakType[1] & 3)]);
             return 1;
          }
          else
          {
	     DBGPrint("\nBreak at 0x%lX due to - INT1 breakpoint (B1)\n",
				 stackFrame->tIP);
             return 1;  /* not one of ours */
          }
       }
       else
       if ((CurrentDR6[processor] & B2_BIT) && (CurrentDR7 & G2_BIT) &&
            Exception == 1)
       {
	  if (BreakGo[2])
          {
	     DBGPrint("\nBreak at 0x%lX due to - GO breakpoint (2)\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if (BreakProceed[2])
          {
	     DBGPrint("\nBreak at 0x%lX due to - Proceed breakpoint (2)\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if (BreakPoints[2] && ConditionalBreakpoint[2])
	  {
	     DBGPrint("\nBreak at 0x%lX due to - breakpoint 2 (%s)\n",
				 stackFrame->tIP,
                                 BreakDescription[(BreakType[2] & 3)]);
	     DBGPrint("expr: %s was TRUE\n", BreakCondition[2]);
             return 1;
	  }
	  else
          if (BreakPoints[2])
          {
	     DBGPrint("\nBreak at 0x%lX due to - breakpoint 2 (%s)\n",
				 stackFrame->tIP,
                                 BreakDescription[(BreakType[2] & 3)]);
             return 1;
          }
          else
          {
	     DBGPrint("\nBreak at 0x%lX due to - INT1 breakpoint (B2)\n",
				 stackFrame->tIP);
             return 1;  /* not one of ours */
          }
       }
       else
       if ((CurrentDR6[processor] & B3_BIT) && (CurrentDR7 & G3_BIT) &&
            Exception == 1)
       {
	  if (BreakGo[3])
          {
	     DBGPrint("\nBreak at 0x%lX due to - GO breakpoint (3)\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if (BreakProceed[3])
          {
	     DBGPrint("\nBreak at 0x%lX due to - Proceed breakpoint (3)\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if (BreakPoints[3] && ConditionalBreakpoint[3])
	  {
	     DBGPrint("\nBreak at 0x%lX due to - breakpoint 3 (%s)\n",
				 stackFrame->tIP,
                                 BreakDescription[(BreakType[3] & 3)]);
	     DBGPrint("expr: %s was TRUE\n", BreakCondition[3]);
             return 1;
	  }
	  else
          if (BreakPoints[3])
          {
	     DBGPrint("\nBreak at 0x%lX due to - breakpoint 3 (%s)\n",
				 stackFrame->tIP,
                                 BreakDescription[(BreakType[3] & 3)]);
             return 1;
          }
          else
          {
	     DBGPrint("\nBreak at 0x%lX due to - INT1 breakpoint (B3)\n",
				 stackFrame->tIP);
             return 1;  /* not one of ours */
          }
       }
       else
       {
	  /* if the last command was a Proceed that was converted into a */
	  /* single step command, report proceed single step */
	  if (lastCommandEntry == 'P' && Exception == 1)
          {
	     DBGPrint("\nBreak at 0x%lX due to - Proceed (single step)\n",
                      stackFrame->tIP);
             return 1;
          }
	  else
          if (lastCommandEntry == 'T' && Exception == 1)
          {
	     DBGPrint("\nBreak at 0x%lX due to - Trace (single step)\n",
                      stackFrame->tIP);
             return 1;
          }
          else
	  if (lastCommandEntry == K_F8 && Exception == 1)
          {
	     DBGPrint("\nBreak at 0x%lX due to - Proceed (single step)\n",
                      stackFrame->tIP);
             return 1;
          }
	  else
          if (lastCommandEntry == K_F7 && Exception == 1)
          {
	     DBGPrint("\nBreak at 0x%lX due to - Trace (single step)\n",
                      stackFrame->tIP);
             return 1;
          }
	  else
          if (lastCommandEntry == K_F6 && Exception == 1)
          {
	     DBGPrint("\nBreak at 0x%lX due to - SSB (step til branch)\n",
                      stackFrame->tIP);
             return 1;
          }
          else
          if (Exception == 3)  /* not our exception */
          {
	     DBGPrint("\nBreak at 0x%lX due to - INT3 breakpoint\n",
				 stackFrame->tIP);
             return 1;
          }
          else
          if (Exception == 1)  /* not our exception, must be gdb */
          {
	     DBGPrint("\nBreak at 0x%lX due to - INT1 breakpoint\n",
				 stackFrame->tIP);
             return 1;
          }
	  else
          if ((Exception < exceptions) &&
              ExceptionDescription[Exception % exceptions])
          {
	     DBGPrint("\nBreak at 0x%lX due to - %s\n",
                    stackFrame->tIP,
                    ExceptionDescription[Exception % exceptions]);
             return 1;
          }
          else
          {
	     DBGPrint("\nBreak at 0x%lX due to - %lu\n",
                    stackFrame->tIP, Exception);
             return 1;
          }
       }
       DBGPrint("\nBreak at 0x%lX due to - Unknown Reason\n",
	        stackFrame->tIP);
       return 1;

}

unsigned long ReasonHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("display break reason\n");
    return 1;
}

unsigned long ReasonDisplay(unsigned char *cmd,
		    StackFrame *stackFrame, unsigned long Exception,
		    DEBUGGER_PARSER *parser)
{
     ConsoleDisplayBreakReason(stackFrame, Exception, get_processor_id(), 0);
     return 1;
}

void ReadStackFrame(void *frame, StackFrame *sf, unsigned long processor)
{
   struct pt_regs *regs = frame;

   sf->tReserved[1] = regs->orig_ax;
   sf->tReserved[2] = (unsigned long)regs;
   sf->tCR3 = ReadCR3();
   sf->tLDT = ReadLDTR();

   sf->tIP = regs->ip;
   sf->tAX = regs->ax;
   sf->tCX = regs->cx;
   sf->tDX = regs->dx;
   sf->tBX = regs->bx;
   sf->tBP = regs->bp;
   sf->tSI = regs->si;
   sf->tDI = regs->di;

#ifdef CONFIG_X86_32
   sf->tSystemFlags = regs->flags;
   sf->tCS = (unsigned short)regs->cs;
   sf->tDS = (unsigned short)regs->ds;
   sf->tES = (unsigned short)regs->es;
   sf->tFS = ReadFS();
   sf->tGS = ReadGS();
   if (user_mode_vm(regs)) 
   {
      sf->tSS = regs->ss;
      sf->tSP = regs->sp;
   } else {
      sf->tSS = __KERNEL_DS;
      sf->tSP = kernel_stack_pointer(regs);
   }
#else
   sf->r8 = regs->r8;
   sf->r9 = regs->r9;
   sf->r10 = regs->r10;
   sf->r11 = regs->r11;
   sf->r12 = regs->r12;
   sf->r13 = regs->r13;
   sf->r14 = regs->r14;
   sf->r15 = regs->r15;

   sf->tSystemFlags = regs->flags;
   sf->tCS = regs->cs;
   sf->tDS = ReadDS();
   sf->tES = ReadES();
   sf->tFS = ReadFS();
   sf->tGS = ReadGS();
   sf->tSS = regs->ss;
   sf->tSP = kernel_stack_pointer(regs);
#endif
   /* save state. */
   memmove((void *)&ReferenceFrame[processor], sf, sizeof(StackFrame));
   return;
}

void ReadTaskFrame(StackFrame *sf, struct task_struct *p)
{
   memset((void *)sf, 0, sizeof(StackFrame));

   sf->tBP = *(unsigned long *)p->thread.sp;
#ifdef CONFIG_X86_32
   sf->tDS = __KERNEL_DS;
   sf->tES = __KERNEL_DS;
   sf->tCS = __KERNEL_CS;
   sf->tSS = __KERNEL_DS;
   sf->tFS = 0xFFFF;
   sf->tGS = 0xFFFF;
   sf->tIP = p->thread.ip;
   sf->tSystemFlags = 0;
#else
   sf->tCS = __KERNEL_CS;
   sf->tSS = __KERNEL_DS;
   sf->tIP = 0;
   sf->tSystemFlags = *(unsigned long *)(p->thread.sp + 8);
#endif
   sf->tSP = p->thread.sp;
   return;
}

void WriteStackFrame(void *frame, StackFrame *sf, unsigned long processor)
{
   struct pt_regs *regs = frame;

   if (ReferenceFrame[processor].tIP != sf->tIP)
      regs->ip = sf->tIP;
   if (ReferenceFrame[processor].tReserved[1] != sf->tReserved[1])
      regs->orig_ax = sf->tReserved[1];
   if (ReferenceFrame[processor].tAX != sf->tAX)
      regs->ax = sf->tAX;
   if (ReferenceFrame[processor].tCX != sf->tCX)
      regs->cx = sf->tCX;
   if (ReferenceFrame[processor].tDX != sf->tDX)
      regs->dx = sf->tDX;
   if (ReferenceFrame[processor].tBX != sf->tBX)
      regs->bx = sf->tBX;
   if (ReferenceFrame[processor].tSP != sf->tSP)
      regs->sp = sf->tSP;
   if (ReferenceFrame[processor].tBP != sf->tBP)
      regs->bp = sf->tBP;
   if (ReferenceFrame[processor].tSI != sf->tSI)
      regs->si = sf->tSI;
   if (ReferenceFrame[processor].tDI != sf->tDI)
      regs->di = sf->tDI;

#ifdef CONFIG_X86_32
   if (ReferenceFrame[processor].tSystemFlags != sf->tSystemFlags)
      regs->flags = sf->tSystemFlags;
   if (ReferenceFrame[processor].tCS != sf->tCS)
      regs->cs = sf->tCS;
   if (ReferenceFrame[processor].tDS != sf->tDS)
      regs->ds = sf->tDS;
   if (ReferenceFrame[processor].tES != sf->tES)
      regs->es = sf->tES;
   if (ReferenceFrame[processor].tFS != sf->tFS)
      regs->fs = sf->tFS;
   if (ReferenceFrame[processor].tGS != sf->tGS)
      regs->gs = sf->tGS;
   if (ReferenceFrame[processor].tSS != sf->tSS)
      regs->ss = sf->tSS;
#else
   if (ReferenceFrame[processor].r8 != sf->r8)
      regs->r8 = sf->r8;
   if (ReferenceFrame[processor].r9 != sf->r9)
      regs->r9 = sf->r9;
   if (ReferenceFrame[processor].r10 != sf->r10)
      regs->r10 = sf->r10;
   if (ReferenceFrame[processor].r11 != sf->r11)
      regs->r11 = sf->r11;
   if (ReferenceFrame[processor].r12 != sf->r12)
      regs->r12 = sf->r12;
   if (ReferenceFrame[processor].r13 != sf->r13)
      regs->r13 = sf->r13;
   if (ReferenceFrame[processor].r14 != sf->r14)
      regs->r14 = sf->r14;
   if (ReferenceFrame[processor].r15 != sf->r15)
      regs->r15 = sf->r15;

   if (ReferenceFrame[processor].tSystemFlags != sf->tSystemFlags)
      regs->flags = sf->tSystemFlags;
   if (ReferenceFrame[processor].tCS != sf->tCS)
      regs->cs = sf->tCS;
   if (ReferenceFrame[processor].tSS != sf->tSS)
      regs->ss = sf->tSS;
#endif
   return;
}

void SetDebugRegisters(void)
{
   register int i;

   for (i = 0; i < 4; i++)
   {
      switch (i)
      {
	 case 0:
	    if (BreakReserved[i])
	    {
	       CurrentDR7 &= 0xFFF0FFFF;
	       CurrentDR7 |= G0_BIT;
	       CurrentDR7 |= ((BreakType[i] << ((i * 4) + 16)) |
			      (BreakLength[i] << ((i * 4) + 18)));
	    }
	    else
	    {
	       CurrentDR7 &= 0xFFF0FFFF;
	       CurrentDR7 &= ~G0_BIT;
	       CurrentDR7 &= ~L0_BIT;
	    }
	    WriteDR0(BreakPoints[i]);
	    break;

	 case 1:
	    if (BreakReserved[i])
	    {
	       CurrentDR7 &= 0xFF0FFFFF;
	       CurrentDR7 |= G1_BIT;
	       CurrentDR7 |= ((BreakType[i] << ((i * 4) + 16)) |
			      (BreakLength[i] << ((i * 4) + 18)));
	    }
	    else
	    {
	       CurrentDR7 &= 0xFF0FFFFF;
	       CurrentDR7 &= ~G1_BIT;
	       CurrentDR7 &= ~L1_BIT;
	    }
	    WriteDR1(BreakPoints[i]);
	    break;

	 case 2:
	    if (BreakReserved[i])
	    {
	       CurrentDR7 &= 0xF0FFFFFF;
	       CurrentDR7 |= G2_BIT;
	       CurrentDR7 |= ((BreakType[i] << ((i * 4) + 16)) |
			      (BreakLength[i] << ((i * 4) + 18)));
	    }
	    else
	    {
	       CurrentDR7 &= 0xF0FFFFFF;
	       CurrentDR7 &= ~G2_BIT;
	       CurrentDR7 &= ~L2_BIT;
	    }
	    WriteDR2(BreakPoints[i]);
	    break;

	 case 3:
	    if (BreakReserved[i])
	    {
	       CurrentDR7 &= 0x0FFFFFFF;
	       CurrentDR7 |= G3_BIT;
	       CurrentDR7 |= ((BreakType[i] << ((i * 4) + 16)) |
			      (BreakLength[i] << ((i * 4) + 18)));
	    }
	    else
	    {
	       CurrentDR7 &= 0x0FFFFFFF;
	       CurrentDR7 &= ~G3_BIT;
	       CurrentDR7 &= ~L3_BIT;
	    }
	    WriteDR3(BreakPoints[i]);
	    break;

      }
   }
   return;

}

void LoadDebugRegisters(void)
{

   register int i;

   WriteDR6(0);  /* clear last exception status */
   for (i = 0; i < 4; i++)
   {
      switch (i)
      {
	 case 0:
	    if (BreakReserved[i])
	       WriteDR0(BreakPoints[i]);
	    break;

	 case 1:
	    if (BreakReserved[i])
	       WriteDR1(BreakPoints[i]);
	    break;

	 case 2:
	    if (BreakReserved[i])
	       WriteDR2(BreakPoints[i]);
	    break;

	 case 3:
	    if (BreakReserved[i])
	       WriteDR3(BreakPoints[i]);
	    break;
      }
   }
   WriteDR7(CurrentDR7);  /* set breakpoint enable/disable state */

}

/*
 *   processor synchronization
 *
 *   We have to handle multiple cpus with active breakpoints
 *   attempting to access the debugger.  We also have to handle
 *   double faulted situations.
 *
 */

unsigned long debug_lock(volatile rlock_t *rlock, unsigned long p)
{
#if defined(CONFIG_SMP)
    if (!spin_trylock_irqsave((spinlock_t *)&rlock->lock, rlock->flags[p]))
    {
       if (rlock->processor == p)
       {
          rlock->count++;
#if MDB_DEBUG_DEBUGGER
          DBGPrint("%i: debug lock(++) count (%lu) state:%s\n",
                   rlock->count, (int)p, procState[ProcessorState[p] & 0xF]);
#endif
       }
       else
       {
#if MDB_DEBUG_DEBUGGER
          DBGPrint("%i: debug trylock FAILED state:%s\n",
                   (int)p, procState[ProcessorState[p] & 0xF]);
#endif
          while (1)
          {
             mdelay(1);
             ProcessorState[p] = PROCESSOR_HOLD;
             while (atomic_read(&focusActive) &&
                   !atomic_read(&traceProcessors[p]))
             {
                cpu_relax();
             }

             if (spin_trylock_irqsave((spinlock_t *)&rlock->lock,
                                      rlock->flags[p]))
                break;
          }
          ProcessorState[p] = PROCESSOR_DEBUG;
          rlock->processor = p;

#if MDB_DEBUG_DEBUGGER
          DBGPrint("%i: debug spinlock SUCCESS state:%s\n",
                   (int)p, procState[ProcessorState[p] & 0xF]);
#endif
       }
    }
    else
       rlock->processor = p;

#if MDB_DEBUG_DEBUGGER
    DBGPrint("%i: debug lock SUCCESS state:%s\n",
             (int)p, procState[ProcessorState[p] & 0xF]);
#endif
#endif /* CONFIG_SMP */
    return 1;
}

void debug_unlock(volatile rlock_t *rlock, unsigned long p)
{
#if defined(CONFIG_SMP)
    if (rlock->processor != p)
    {
#if MDB_DEBUG_DEBUGGER
       DBGPrint("%i: debug unlock FAILED state:%s (%lu/%lu)\n",
                (int)p, procState[ProcessorState[p] & 0xF],
                 rlock->processor, p);
#endif
    }

    if (rlock->count)
    {
       rlock->count--;
#if MDB_DEBUG_DEBUGGER
       DBGPrint("%i: debug unlock(--) count (%lu) state:%s\n",
                (int)p, rlock->count, procState[ProcessorState[p] & 0xF]);
#endif
    }
    else
    {
       rlock->processor = -1;
       spin_unlock_irqrestore((spinlock_t *)&rlock->lock, rlock->flags[p]);
    }
#if MDB_DEBUG_DEBUGGER
    DBGPrint("%i: debug unlock count (%lu) state:%s\n",
             (int)p, rlock->count, procState[ProcessorState[p] & 0xF]);
#endif
#endif /* CONFIG_SMP */
    return;
}

unsigned long debug_rlock(volatile rlock_t *rlock, unsigned long p)
{
#if defined(CONFIG_SMP)
    if (!spin_trylock_irqsave((spinlock_t *)&rlock->lock, rlock->flags[p]))
    {
       if (rlock->processor == p)
       {
          rlock->count++;
#if MDB_DEBUG_DEBUGGER
          DBGPrint("%i: debug lock(++) count (%lu) state:%s\n",
                   rlock->count, (int)p, procState[ProcessorState[p] & 0xF]);
#endif
       }
       else
       {
#if MDB_DEBUG_DEBUGGER
          DBGPrint("%i: debug trylock FAILED state:%s\n",
                   (int)p, procState[ProcessorState[p] & 0xF]);
#endif
          while (1)
          {
             mdelay(1);
             cpu_relax();
             if (spin_trylock_irqsave((spinlock_t *)&rlock->lock,
                                      rlock->flags[p]))
                break;
          }
          rlock->processor = p;

#if MDB_DEBUG_DEBUGGER
          DBGPrint("%i: debug spinlock SUCCESS state:%s\n",
                   (int)p, procState[ProcessorState[p] & 0xF]);
#endif
       }
    }
    else
       rlock->processor = p;

#if MDB_DEBUG_DEBUGGER
    DBGPrint("%i: debug lock SUCCESS state:%s\n",
             (int)p, procState[ProcessorState[p] & 0xF]);
#endif
#endif /* CONFIG_SMP */
    return 1;
}

void debug_unrlock(volatile rlock_t *rlock, unsigned long p)
{
#if defined(CONFIG_SMP)
    if (rlock->processor != p)
    {
#if MDB_DEBUG_DEBUGGER
       DBGPrint("%i: debug unlock FAILED state:%s (%lu/%lu)\n",
                (int)p, procState[ProcessorState[p] & 0xF],
                 rlock->processor, p);
#endif
    }

    if (rlock->count)
    {
       rlock->count--;
#if MDB_DEBUG_DEBUGGER
       DBGPrint("%i: debug unlock(--) count (%lu) state:%s\n",
                (int)p, rlock->count, procState[ProcessorState[p] & 0xF]);
#endif
    }
    else
    {
       rlock->processor = -1;
       spin_unlock_irqrestore((spinlock_t *)&rlock->lock, rlock->flags[p]);
    }
#if MDB_DEBUG_DEBUGGER
    DBGPrint("%i: debug unlock count (%lu) state:%s\n",
             (int)p, rlock->count, procState[ProcessorState[p] & 0xF]);
#endif
#endif /* CONFIG_SMP */
    return;
}

unsigned long StopProcessorsExclSelf(unsigned long self)
{
#if defined(CONFIG_SMP) && !defined(CONFIG_X86_ELAN)
   register unsigned long failed;
   register int i;

#if MDB_DEBUG_DEBUGGER
   if (atomic_read(&debuggerActive) > 1)
   {
      DBGPrint("%i: stop processors IPI waiters:%i state:%s\n",
               (int)self, (int)atomic_read(&debuggerActive),
               procState[ProcessorState[self] & 0xF]);
   }
   DBGPrint("%i: stop processors ENTER state:%s\n",
             (int)self, procState[ProcessorState[self] & 0xF]);
#endif

   /*
      You need to delay before issuing NMI and allow any previously
      released processors to exit and issue their IRETD before
      triggering another NMI event or exception.

      This fix seems to avoid lockup during APIC ICR write on some SMT
      capable and dual core laptop computers (Acer 9410). if an ICR xcall
      is issued before a target processor has exited an active
      exception or if a processor inside an active INT1 exception
      receives a directed NMI from the local APIC, it can result in
      a hard hang.   The reason for this behavior has not been fully
      investigated.

      This lockup occurs if more than one processor is currently handling
      an INT1 debugger exception and one of the processors gets an NMI
      while still inside the INT1 exception (nested exceptions).  It
      does not happen all the time and appears timing dependent.

      The symptom is a hard bus hang while attempting to write an ICR
      command to the local APIC ICR register.  The check for a busy bit
      in the ICR register succeeds and allows the ICR write to occur
      prior to the lockup.
   */
   mdelay(1);

   for (i = 0; i < MAX_PROCESSORS; i++)
   {
      if (cpu_online(i))
      {
         if (i != self)
         {
            /* do not NMI a procesor already spinning inside the
               debugger.  It may result in a system lockup. */
            if (!atomic_read(&debuggerProcessors[i]))
            {
	       ProcessorHold[i] = 1;
               barrier();
#if MDB_DEBUG_DEBUGGER
               DBGPrint("%i:send_IPI_mask ENTER cpu:%lu state:%s\n",
                        (int)self, i, procState[ProcessorState[i] & 0xF]);
#endif
               apic->send_IPI_mask(cpumask_of(i), APIC_DM_NMI);
#if MDB_DEBUG_DEBUGGER
               DBGPrint("%i:send_IPI_mask EXIT cpu:%lu state:%s\n",
                        (int)self, i, procState[ProcessorState[i] & 0xF]);
#endif
            }
         }
      }
   }

#if 0
   /* This code can result in hard system lockup on certain laptop models
    * during NMI of the other processors. The cause has not been
    * determined. */
   DBGPrint("%i: Send_IPI_allbutself ...", (int)self);
   send_IPI_allbutself(APIC_DM_NMI);
   DBGPrint(" completed\n");
#endif

   for (i = 0, failed=0; i < MAX_PROCESSORS; i++)
   {
      if (cpu_online(i))
      {
         if (i != self)
         {
	    register unsigned long msecs = 1000;

	    while (!atomic_read(&debuggerProcessors[i]) && msecs)
	    {
	       mdelay(1);
	       msecs--;
	    }

	    if (!msecs)
	    {
	       failed++;
	       DBGPrint("Processor %i could not be halted state:%s\n",
                        (int)i, procState[ProcessorState[i] & 0xF]);
	    }
         }
      }
   }
#if MDB_DEBUG_DEBUGGER
   DBGPrint("%i: stop processors EXITED state:%s\n",
             (int)self, procState[ProcessorState[self] & 0xF]);
#endif

   return (unsigned long) failed;
#else
   return 0;
#endif
}

unsigned long FreeProcessorsExclSelf(unsigned long self)
{
#if defined(CONFIG_SMP) && !defined(CONFIG_X86_ELAN)
   register int i;

#if MDB_DEBUG_DEBUGGER
   DBGPrint("%i: release processors ENTER state:%s\n",
             (int)self, procState[ProcessorState[self] & 0xF]);
#endif

   for (i = 0; i < MAX_PROCESSORS; i++)
   {
      if (ProcessorState[i] != PROCESSOR_HOLD)
      {
         ProcessorState[i] = PROCESSOR_RESUME;
         barrier();
      }
   }

#if MDB_DEBUG_DEBUGGER
   DBGPrint("%i: release processors EXITED state:%s\n",
             (int)self, procState[ProcessorState[self] & 0xF]);
#endif

   return i;
#else
   return MAX_PROCESSORS;
#endif

}

unsigned long WaitRestartExclSelf(unsigned long self)
{
#if defined(CONFIG_SMP)
   register unsigned long failed;
   register int i;

#if MDB_DEBUG_DEBUGGER
   DBGPrint("%i: wait processors ENTERED state:%s\n",
             (int)self, procState[ProcessorState[self] & 0xF]);
#endif

   for (i = 0, failed=0; i < MAX_PROCESSORS; i++)
   {
      if (cpu_online(i))
      {
         if (i != self)
         {
	    register unsigned long msecs = 1000;

            smp_rmb();
	    while (atomic_read(&nmiProcessors[i]) && msecs)
	    {
	       mdelay(1);
	       msecs--;
	    }

	    if (!msecs)
	    {
	       failed++;
	       DBGPrint("Processor %i did not restart state:%s\n",
                        (int)i, procState[ProcessorState[i] & 0xF]);
	    }
         }
      }
   }

#if MDB_DEBUG_DEBUGGER
   DBGPrint("%i: wait processors EXITED state:%s\n",
             (int)self, procState[ProcessorState[self] & 0xF]);
#endif

   return (unsigned long) failed;
#else
   return 0;
#endif
}

unsigned long enter_debugger(unsigned long exception, StackFrame *stackFrame,
                             unsigned long processor)
{
    register unsigned long retCode;

    if (debug_lock(&debug_mutex, processor))
    {
       /*  if the processors were already held in the debugger due to a
        *  trace, ssb, or proceed session on a focus processor, do not
        *  nest an xcall NMI or signal (not if you can help it). */
       if (!atomic_read(&traceProcessors[processor]))
          StopProcessorsExclSelf(processor);

       retCode = debugger_command_entry(processor, exception, stackFrame);

       /*  do not release the processors for active trace, ssb, or proceed
        *  sessions on a focus processor. */
       if (!atomic_read(&traceProcessors[processor]))
       {
          FreeProcessorsExclSelf(processor);
          WaitRestartExclSelf(processor);
       }
       debug_unlock(&debug_mutex, processor);
       touch_softlockup_watchdog();
       clocksource_touch_watchdog();
       return retCode;
    }
    touch_softlockup_watchdog();
    clocksource_touch_watchdog();
    return 0;
}


unsigned long debugger_entry(unsigned long Exception, StackFrame *stackFrame,
                     unsigned long processor)
{
    register unsigned long retCode = 1;
    unsigned char *cmd;
    unsigned long valid;

    atomic_inc(&debuggerActive);
    atomic_inc(&debuggerProcessors[processor]);

    ProcessorState[processor] = PROCESSOR_DEBUG;
    CurrentFrame[processor] = stackFrame;

    WriteDR7(0);  /* disable breakpoints while debugger is running */
    CurrentDR6[processor] = ReadDR6();

    if (fpu_present())
       save_npx(&npx[processor]);

MDBLoop:;
    switch (Exception)
    {
          case 1:/* int 1 debug exception */
	    if (BreakMask[processor])
	    {
	       stackFrame->tSystemFlags &= ~SINGLE_STEP;
	       stackFrame->tSystemFlags |= RESUME;
	       break;
	    }
	    else
	    if ((CurrentDR6[processor] & B0_BIT) &&
                (CurrentDR7 & G0_BIT) &&
                (ConditionalBreakpoint[0]))
	    {
	       cmd = (unsigned char *)&BreakCondition[0][0];
	       if (!EvaluateExpression(stackFrame, &cmd, &valid))
	       {
		  stackFrame->tSystemFlags &= ~SINGLE_STEP;
		  stackFrame->tSystemFlags |= RESUME;
		  break;
	       }
	    }
	    else
	    if ((CurrentDR6[processor] & B1_BIT) &&
                (CurrentDR7 & G1_BIT) &&
                (ConditionalBreakpoint[1]))
	    {
	       cmd = (unsigned char *)&BreakCondition[1][0];
	       if (!EvaluateExpression(stackFrame, &cmd, &valid))
	       {
		  stackFrame->tSystemFlags &= ~SINGLE_STEP;
		  stackFrame->tSystemFlags |= RESUME;
		  break;
	       }
	    }
	    else
	    if ((CurrentDR6[processor] & B2_BIT) &&
                (CurrentDR7 & G2_BIT) &&
                (ConditionalBreakpoint[2]))
	    {
	       cmd = (unsigned char *)&BreakCondition[2][0];
	       if (!EvaluateExpression(stackFrame, &cmd, &valid))
	       {
		  stackFrame->tSystemFlags &= ~SINGLE_STEP;
		  stackFrame->tSystemFlags |= RESUME;
		  break;
	       }
	    }
	    else
	    if ((CurrentDR6[processor] & B3_BIT) &&
                (CurrentDR7 & G3_BIT) &&
                (ConditionalBreakpoint[3]))
	    {
	       cmd = (unsigned char *)&BreakCondition[3][0];
	       if (!EvaluateExpression(stackFrame, &cmd, &valid))
	       {
		  stackFrame->tSystemFlags &= ~SINGLE_STEP;
		  stackFrame->tSystemFlags |= RESUME;
		  break;
	       }
	    }
	    enter_debugger(Exception, stackFrame, processor);
	    break;


	 case 3:/* int 3 breakpoint */
	    if (BreakMask[processor])
	    {
	       stackFrame->tSystemFlags &= ~SINGLE_STEP;
	       stackFrame->tSystemFlags |= RESUME;
	       break;
	    }
	    enter_debugger(Exception, stackFrame, processor);
	    break;

         case 2: /* nmi */
            barrier();
            if (ProcessorHold[processor])  /* hold processor */
            {
               ProcessorHold[processor] = 0;
	       ProcessorState[processor] = PROCESSOR_SUSPEND;

               /* processor suspend loop */
               smp_wmb();
               atomic_inc(&nmiProcessors[processor]);
	       while ((ProcessorState[processor] != PROCESSOR_RESUME) &&
	              (ProcessorState[processor] != PROCESSOR_SWITCH))
               {
                  barrier();
	          if ((ProcessorState[processor] == PROCESSOR_RESUME) ||
	              (ProcessorState[processor] == PROCESSOR_SWITCH))
                  {
                     break;
                  }
                  cpu_relax();
               }
               atomic_dec(&nmiProcessors[processor]);
               touch_softlockup_watchdog();
               clocksource_touch_watchdog();

               if (ProcessorState[processor] == PROCESSOR_SWITCH)
	          enter_debugger(21, stackFrame, processor);
               break;
            }
            else   /* all other nmi exceptions fall through to here */
	       enter_debugger(Exception, stackFrame, processor);
            break;

	 default:
	    enter_debugger(Exception, stackFrame, processor);
	    break;
    }

    if (ProcessorHold[processor])
    {
       Exception = 2;
       goto MDBLoop;
    }

    LoadDebugRegisters();

    if (fpu_present())
       load_npx(&npx[processor]);

    CurrentFrame[processor] = 0;
    ProcessorState[processor] = PROCESSOR_ACTIVE;

    atomic_dec(&debuggerProcessors[processor]);
    atomic_dec(&debuggerActive);
    return retCode;

}

void InitializeDebuggerRegisters(void)
{
   CurrentDR7 = (DR7DEF | GEXACT | LEXACT); /* set mode to GLOBAL EXACT */
   WriteDR0(0);                      /* clear out DR0-DR6 */
   WriteDR1(0);
   WriteDR2(0);
   WriteDR3(0);
   WriteDR6(0);
   WriteDR7(CurrentDR7);            /* set DR7 register */
}

void ClearDebuggerRegisters(void)
{
   WriteDR0(0);   /* clear out all breakpoints and breakpoint */
   WriteDR1(0);   /* registers DR0-DR7 */
   WriteDR2(0);
   WriteDR3(0);
   WriteDR6(0);
   WriteDR7(0);
}

