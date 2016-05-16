
/***************************************************************************
 *
 *   Copyright (c) 2000-2016 Jeff V. Merkey  All Rights Reserved.
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
 *   with the terms specified in the GNU Public License.
 *
 *   AUTHOR   :  Jeff V. Merkey
 *   DESCRIP  :  Minimal Linux Debugger
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
#include <linux/ctype.h>
#include <linux/keyboard.h>
#include <linux/console.h>
#include <linux/serial_reg.h>
#include <linux/uaccess.h>
#include <linux/nmi.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/clocksource.h>
#include <linux/rcupdate.h>
#include <linux/hw_breakpoint.h>
#include <linux/tty.h>
#include <linux/vt_kern.h>
#include <asm/segment.h>
#include <linux/atomic.h>
#include <asm/msr.h>
#include <asm/ptrace.h>
#include <asm/debugreg.h>

#if IS_ENABLED(CONFIG_SMP)
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

unsigned char *ia_flags[] = {
	"CF", 0, "PF", 0, "AF",    0, "ZF", "SF", "TF", "IF", "DF", "OF",
	0,    0, "NT", 0, "RF", "VM", "AC", "VIF", "VIP", "ID",    0,    0,
	0,
};

unsigned char *break_description[] = {
	"EXECUTE",  "WRITE",  "IOPORT",  "READ/WRITE",
};

unsigned char *break_length_description[] = {
	": 1 BYTE",  ": 2 BYTE",  ": 8 BYTE",  ": 4 BYTE",
};

unsigned char *exception_description[] = {
	"Divide By Zero",                 /*  0 */
	"Debugger exception (INT1)",      /*  1 */
	"Non-Maskable Interrupt",         /*  2 */
	"Debugger Breakpoint (INT3)",     /*  3 */
	"Overflow exception",             /*  4 */
	"Bounds Check",                   /*  5 */
	"Invalid Opcode",                 /*  6 */
	"No Coprocessor",                 /*  7 */
	"Double Fault",                   /*  8 */
	"Cops Error",                     /*  9 */
	"Invalid Task State Segment",     /*  10 */
	"Segment Not Present",            /*  11 */
	"Stack exception",                /*  12 */
	"General Protection",             /*  13 */
	"Page Fault",                     /*  14 */
	"Invalid Interrupt",              /*  15 */
	"Coprocessor Error",              /*  16 */
	"Alignment Check",                /*  17 */
	"Machine Check",                  /*  18 */
	"Enter Debugger Request",         /*  19 */
	"Unvectored exception",           /*  20 */
	"Directed NMI Breakpoint",        /*  21 */
	"Panic"                           /*  22 */
};

unsigned long exceptions = ARRAY_SIZE(exception_description);

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
#define PROCESSOR_WAIT        8

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

rlock_t debug_mutex = { -1, 0 };
DEFINE_SPINLOCK(debuglock);

atomic_t focus_active;  /* cpus is focus */
atomic_t debugger_active;  /* cpus in the debugger */

unsigned char *proc_state[] = {
	"PROCESSOR_INACTIVE", "PROCESSOR_ACTIVE  ", "PROCESSOR_SUSPEND ",
	"PROCESSOR_RESUME  ", "PROCESSOR_DEBUG   ", "PROCESSOR_SHUTDOWN",
	"PROCESSOR_IPI     ", "PROCESSOR_SWITCH  ", "PROCESSOR_WAIT    ",
	"?                 ", "?                 ", "?                 ",
	"?                 ", "?                 ", "?                 ",
	"?                 "
};

unsigned char *category_strings[] = {
	"Running Program Code",                /* 0 */
	"Examining and Changing Memory",       /* 1 */
	"Examining and Disassembling Code",    /* 2 */
	"Using Breakpoints",                   /* 3 */
	"Stack Backtracing",                   /* 4 */
	"Searching and Displaying Symbols",    /* 5 */
	"Numerical Exprs",		       /* 6 */
	"SMP Debugging",                       /* 7 */
	"Examining Hardware",                  /* 8 */
	"Configuring the Debugger",            /* 9 */
	"System Dependent",                    /* 10 */
	NULL,                                  /* 11 */
	NULL                                   /* 13 */
};

/* debugger commands */

dbg_parser back_trace_all_pid_dp = {
	0, 0, back_trace_all_pid, back_trace_help, 0, "BTA", 0, 0,
	"display stack backtrace for all processes", 0, 4 };

dbg_parser back_trace_pid_dp = {
	0, 0, back_trace_pid, back_trace_help, 0, "BTP", 0, 0,
	"display stack backtrace by pid", 0, 4 };

dbg_parser back_trace_stack_dp = {
	0, 0, back_trace_stack, back_trace_help, 0, "BT", 0, 0,
	"display stack backtrace by address", 0, 4 };

dbg_parser cpu_frame_dp = {
	0, 0, list_processor_frame, processor_command_help, 0, "LR", 0, 0,
	"display cpu registers", 0, 7};

dbg_parser processor_dp = {
	0, 0, display_processor_status, display_processor_status_help, 0,
	"PROCESSORS", 0, 0, "display processor status", 0, 7 };

dbg_parser sh_sp = {
	0, 0, display_debugger_help, display_debugger_help_help, 0, "HELP",
	0, 0, "this help screen (type HELP <command> for specific help)",
	0, 10 };

dbg_parser help_dp = {
	0, 0, display_debugger_help, display_debugger_help_help, 0, "H", 0, 0,
	"this help screen", 0, 10};

dbg_parser clear_screen_dp = {
	0, 0, clear_debugger_screen, clear_screen_help, 0, "CLS", 0, 0,
	"clear the screen", 0, 10};

dbg_parser ascii_table_dp = {
	0, 0, display_asc_table, asc_table_help, 0, "A", 0, 0,
	"display ASCII Table", 0, 10};

dbg_parser display_toggle_1 = {
	0, 0, display_toggle_all, display_toggle_help, 0, ".TOGGLE", 0, 0,
	"show all current toggle settings", 0, 9 };

dbg_parser display_toggle_2 = {
	0, 0, display_toggle_all, display_toggle_help, 0, "TOGGLE", 0, 0,
	"show all current toggle settings", 0, 9 };

dbg_parser TB_toggle_dp = {
	0, 0, process_tb_toggle, display_toggle_help, 0, ".TB", 0, 0,
	"toggle disable breakpoints in user address space (ON | OFF)", 0, 9 };

dbg_parser TU_toggle_dp = {
	0, 0, process_tu_toggle, display_toggle_help, 0, ".TU", 0, 0,
	"toggles unasm debug display (ON | OFF)", 0, 9 };

dbg_parser TD_toggle_dp = {
	0, 0, process_td_toggle, display_toggle_help, 0, ".TD", 0, 0,
	"toggles full dereference display (ON | OFF)", 0, 9 };

dbg_parser TL_toggle_dp = {
	0, 0, process_tl_toggle, display_toggle_help, 0, ".TL", 0, 0,
	"toggles source line display (ON | OFF)", 0, 9 };

dbg_parser TG_toggle_dp = {
	0, 0, process_tg_toggle, display_toggle_help, 0, ".TG", 0, 0,
	"toggles general registers (ON | OFF)", 0, 9 };

dbg_parser TC_toggle_dp = {
	0, 0, process_tc_toggle, display_toggle_help, 0, ".TC", 0, 0,
	"toggles control registers (ON | OFF)", 0, 9 };

dbg_parser TN_toggle_dp = {
	0, 0, process_tn_toggle, display_toggle_help, 0, ".TN", 0, 0,
	"toggles coprocessor registers (ON | OFF)", 0, 9 };

dbg_parser TR_toggle_dp = {
	0, 0, process_tr_toggle, display_toggle_help, 0, ".TR", 0, 0,
	"toggles display of break reason (ON | OFF)", 0, 9 };

dbg_parser TS_toggle_dp = {
	0, 0, process_ts_toggle, display_toggle_help, 0, ".TS", 0, 0,
	"toggles segment registers (ON | OFF)", 0, 9 };

dbg_parser TA_toggle_dp = {
	0, 0, process_ta_toggle, display_toggle_help, 0, ".TA", 0, 0,
	"toggles all registers (ON | OFF)", 0, 9 };

dbg_parser toggle_user = {
	0, 0, process_toggle_user, display_toggle_help, 0, ".TM", 0, 0,
	"toggle memory reads/write to map ranges < PAGE_OFFSET", 0, 9 };

dbg_parser reason_dp = {
	0, 0, reason_display, reason_help, 0, ".A", 0, 0,
	"display break reason", 0, 0 };

dbg_parser T_toggle_dp = {
	0, 0, tss_display, tss_display_help, 0, ".T", 0, 0,
	"display task state segment (tss)", 0, 8 };

dbg_parser version_dp = {
	0, 0, display_debugger_version, display_debugger_version_help, 0,
	".V", 0, 0, "display version info", 0, 10 };

#if IS_ENABLED(CONFIG_MODULES)
dbg_parser lsmod_pe_1 = {
	0, 0, list_modules, list_modules_help, 0, ".M", 0, 0,
	"list loaded modules", 0, 10 };

dbg_parser lsmod_pe_2 = {
	0, 0, list_modules, list_modules_help, 0, "LSMOD", 0, 0,
	"list loaded modules", 0, 10 };

dbg_parser rmmod_dp = {
	0, 0, unload_module, list_modules_help, 0, "RMMOD", 0, 0,
	"unload module", 0, 10 };
#endif

dbg_parser reboot_dp = {
	0, 0, reboot_system, reboot_system_help, 0, "REBOOT", 0, 0,
	"reboot host system", 0, 10 };

dbg_parser kprocess_1 = {
	0, 0, display_kprocess, display_kprocess_help, 0, ".P", 0, 0,
	"display kernel processes", 0, 10 };

dbg_parser kprocess_2 = {
	0, 0, display_kprocess, display_kprocess_help, 0, "PS", 0, 0,
	"display kernel processes", 0, 10 };

dbg_parser all_symbols_dp = {
	0, 0, display_symbols, display_symbols_help, 0, "SYMBOL", 0, 0,
	"display symbol(s)", 0, 5 };

dbg_parser symbols_dp = {
	0, 0, display_symbols, display_symbols_help, 0, ".Z", 0, 0,
	"display symbol(s)", 0, 5 };

dbg_parser control_dp = {
	0, 0, control_registers, display_registers_help, 0, "RC", 0, 0,
	"display control registers", 0, 8 };

dbg_parser all_dp = {
	0, 0, display_all_registers, display_registers_help, 0, "RA", 0, 0,
	"display all registers", 0, 8 };

dbg_parser segment_dp = {
	0, 0, segment_registers, display_registers_help, 0, "RS", 0, 0,
	"display segment registers", 0, 8 };

dbg_parser numeric_dp = {
	0, 0, display_numeric_registers, display_registers_help, 0, "RN", 0, 0,
	"display coprocessor/MMX registers", 0, 8 };

dbg_parser general_dp = {
	0, 0, general_registers, display_registers_help, 0, "RG", 0, 0,
	"display general registers", 0, 8 };

dbg_parser default_dp = {
	0, 0, display_default_registers, display_registers_help, 0, "R", 0, 0,
	"display registers for a processor", 0, 8 };

dbg_parser search_memory_b_dp = {
	0, 0, search_memory_b, search_memory_help, 0, "SB", 0, 0,
	"search memory for pattern (bytes)", 0, 1 };

dbg_parser search_memory_w_dp = {
	0, 0, search_memory_w, search_memory_help, 0, "SW", 0, 0,
	"search memory for pattern (words)", 0, 1 };

dbg_parser search_memory_d_dp = {
	0, 0, search_memory_d, search_memory_help, 0, "SD", 0, 0,
	"search memory for pattern (dwords)", 0, 1 };

dbg_parser search_memory_q_dp = {
	0, 0, search_memory_q, search_memory_help, 0, "SQ", 0, 0,
	"search memory for pattern (qwords)", 0, 1 };

dbg_parser change_word_dp = {
	0, 0, change_word_value, change_memory_help, 0, "CW", 0, 0,
	"change words at address", 0, 1 };

dbg_parser change_double_dp = {
	0, 0, change_double_value, change_memory_help, 0, "CD", 0, 0,
	"change dwords at address", 0, 1 };

dbg_parser change_quad_dp = {
	0, 0, change_quad_value, change_memory_help, 0, "CQ", 0, 0,
	"change qwords at address", 0, 1 };

dbg_parser change_byte_dp = {
	0, 0, change_byte_value, change_memory_help, 0, "CB", 0, 0,
	"change bytes at address", 0, 1 };

dbg_parser change_default_dp = {
	0, 0, change_default_value, change_memory_help, 0, "C", 0, 0,
	"change bytes at address", 0, 1 };

dbg_parser close_symbols_dp = {
	0, 0, display_close_symbols, display_close_help, 0, "?", 0, 0,
	"display closest symbols to <address>", 0, 5 };

dbg_parser walk_dp = {
	0, 0, debugger_walk_stack, display_dump_help, 0, "W", 0, 0,
	"display symbols on the stack", 0, 4 };

dbg_parser dump_linked_dp = {
	0, 0, debugger_dump_linked_list, display_dump_help, 0, "DL", 0, 0,
	"dump linked list", 0, 1 };

dbg_parser dump_stack_dp = {
	0, 0, debugger_dump_stack, display_dump_help, 0, "DS", 0, 0,
	"dump stack", 0, 4};

dbg_parser dump_double_stack_dp = {
	0, 0, debugger_dump_double_stack, display_dump_help, 0, "DDS", 0, 0,
	"dump stack double word", 0, 4 };

dbg_parser dump_quad_stack_dp = {
	0, 0, debugger_dump_quad_stack, display_dump_help, 0, "DQS", 0, 0,
	"dump stack quad word", 0, 4 };

dbg_parser dump_quad_dp = {
	0, 0, debugger_dump_quad, display_dump_help, 0, "DQ", 0, 0,
	"dump memory as quad words", 0, 1 };

dbg_parser dump_double_dp = {
	0, 0, debugger_dump_double, display_dump_help, 0, "DD", 0, 0,
	"dump memory as double words", 0, 1 };

dbg_parser dump_word_dp = {
	0, 0, debugger_dump_word, display_dump_help, 0, "DW", 0, 0,
	"dump memory as words", 0, 1 };

dbg_parser dump_default_dp = {
	0, 0, debugger_dump_byte, display_dump_help, 0, "D", 0, 0,
	"dump memory as bytes", 0, 1 };

dbg_parser dump_byte_dp = {
	0, 0, debugger_dump_byte, display_dump_help, 0, "DB", 0, 0,
	"dump memory as bytes", 0, 1 };

dbg_parser dump_byte_phys_dp = {
	0, 0, debugger_dump_byte_phys, display_dump_help, 0, "DPB", 0, 0,
	"dump physical address as bytes", 0, 1 };

dbg_parser dump_quad_phys_dp = {
	0, 0, debugger_dump_quad_phys, display_dump_help, 0, "DPQ", 0, 0,
	"dump physical address as quad words", 0, 1 };

dbg_parser dump_double_phys_dp = {
	0, 0, debugger_dump_double_phys, display_dump_help, 0, "DPD", 0, 0,
	"dump physical address as double words", 0, 1 };

dbg_parser dump_word_phys_dp = {
	0, 0, debugger_dump_word_phys, display_dump_help, 0, "DPW", 0, 0,
	"dump physical address as words", 0, 1 };

dbg_parser dump_default_phys_dp = {
	0, 0, debugger_dump_byte_phys, display_dump_help, 0, "DP", 0, 0,
	"dump physical address as bytes", 0, 1 };

dbg_parser diss_16_dp = {
	0, 0, process_disasm_16, display_disassemble_help, 0, "UU", 0, 0,
	"unassemble code (16-bit)", 0, 2 };

dbg_parser diss_32_dp = {
	0, 0, process_disasm_32, display_disassemble_help, 0, "UX", 0, 0,
	"unassemble code (32-bit)", 0, 2 };

dbg_parser diss_any_dp = {
	0, 0, process_disassemble_any, display_disassemble_help, 0, "U", 0, 0,
	"unassemble code (INTEL format)", 0, 2 };

dbg_parser id_dp = {
	0, 0, process_disassemble_att, display_disassemble_help, 0, "ID", 0, 0,
	"unassemble code (GNU format)", 0, 2 };

dbg_parser proceed_dp = {
	0, 0, process_proceed, execute_command_help, 0, "P", 0, 0,
	"proceed (step over loops and function calls)", -1, 0 };

dbg_parser trace_dp = {
	0, 0, process_trace, execute_command_help, 0, "T", 0, 0,
	"trace", -1, 0 };

dbg_parser single_step_dp = {
	0, 0, process_trace, execute_command_help, 0, "S", 0, 0,
	"single step", -1, 0 };

dbg_parser trace_ss_dp = {
	0, 0, process_trace, execute_command_help, 0, "SS", 0, 0,
	"single step", -1, 0 };

dbg_parser trace_ssb_dp = {
	0, 0, process_trace_ssb, execute_command_help, 0, "SSB", 0, 0,
	"single step til branch", -1, 0 };

dbg_parser G_dp = {
	0, 0, process_go, execute_command_help, 0, "G", 0, 0,
	"g or g til <address> match", -1, 0 };

dbg_parser go_dp = {
	0, 0, process_go, execute_command_help, 0, "GO", 0, 0,
	"go or go til <address> match", -1, 0 };

dbg_parser Q_dp = {
	0, 0, process_go, execute_command_help, 0, "Q", 0, 0,
	"quit debugger until <address> match", -1, 0 };

dbg_parser X_dp = {
	0, 0, process_go, execute_command_help, 0, "X", 0, 0,
	"exit debugger until <address> match", -1, 0 };

dbg_parser break_processor_dp = {
	0, 0, break_processor, processor_command_help, 0, "CPU", 0, 0,
	"switch processor", -1, 7 };

dbg_parser list_processors_dp = {
	0, 0, list_processors, processor_command_help, 0, "LCPU", 0, 0,
	"list processors", 0, 7 };

dbg_parser ORIGEAX_dp = {
	0, 0, change_origeax_register, display_eax_help, 0, "ORGEAX", 0, -1,
	"", 0, -1 };

dbg_parser AL_dp = {
	0, 0, change_eax_register, display_eax_help, 0, "AL", 0, -1,
	"", 0, -1 };

dbg_parser BL_dp = {
	0, 0, change_ebx_register, display_ebx_help, 0, "BL", 0, -1,
	"", 0, -1 };

dbg_parser CL_dp = {
	0, 0, change_ecx_register, display_ecx_help, 0, "CL", 0, -1,
	"", 0, -1 };

dbg_parser DL_dp = {
	0, 0, change_edx_register, display_edx_help, 0, "DL", 0, -1,
	"", 0, -1 };

dbg_parser AX_dp = {
	0, 0, change_eax_register, display_eax_help, 0, "AX", 0, -1,
	"", 0, -1 };

dbg_parser BX_dp = {
	0, 0, change_ebx_register, display_ebx_help, 0, "BX", 0, -1,
	"", 0, -1 };

dbg_parser CX_dp = {
	0, 0, change_ecx_register, display_ecx_help, 0, "CX", 0, -1,
	"", 0, -1 };

dbg_parser DX_dp = {
	0, 0, change_edx_register, display_edx_help, 0, "DX", 0, -1,
	"", 0, -1 };

dbg_parser EAX_dp = {
	0, 0, change_eax_register, display_eax_help, 0, "EAX", 0, -1,
	"", 0, -1 };

dbg_parser EBX_dp = {
	0, 0, change_ebx_register, display_ebx_help, 0, "EBX", 0, -1,
	"", 0, -1 };

dbg_parser ECX_dp = {
	0, 0, change_ecx_register, display_ecx_help, 0, "ECX", 0, -1,
	"", 0, -1 };

dbg_parser EDX_dp = {
	0, 0, change_edx_register, display_edx_help, 0, "EDX", 0, -1,
	"", 0, -1 };

dbg_parser ESI_dp = {
	0, 0, change_esi_register, display_esi_help, 0, "ESI", 0, -1,
	"", 0, -1 };

dbg_parser EDI_dp = {
	0, 0, change_edi_register, display_edi_help, 0, "EDI", 0, -1,
	"", 0, -1 };

dbg_parser EBP_dp = {
	0, 0, change_ebp_register, display_ebp_help, 0, "EBP", 0, -1,
	"", 0, -1 };

dbg_parser ESP_dp = {
	0, 0, change_esp_register, display_esp_help, 0, "ESP", 0, -1,
	"", 0, -1 };

dbg_parser EIP_dp = {
	0, 0, change_eip_register, display_eip_help, 0, "EIP", 0, -1,
	"", 0, -1 };

#if IS_ENABLED(CONFIG_X86_64)
dbg_parser RAX_dp = {
	0, 0, change_rax_register, display_rax_help, 0, "RAX", 0, -1,
	"", 0, -1 };

dbg_parser ORIGRAX_dp = {
	0, 0, change_origrax_register, display_rax_help, 0, "ORGRAX", 0, -1,
	"", 0, -1 };

dbg_parser RBX_dp = {
	0, 0, change_rbx_register, display_rbx_help, 0, "RBX", 0, -1,
	"", 0, -1 };

dbg_parser RCX_dp = {
	0, 0, change_rcx_register, display_rcx_help, 0, "RCX", 0, -1,
	"", 0, -1 };

dbg_parser RDX_dp = {
	0, 0, change_rdx_register, display_rdx_help, 0, "RDX", 0, -1,
	"", 0, -1 };

dbg_parser RSI_dp = {
	0, 0, change_rsi_register, display_rsi_help, 0, "RSI", 0, -1,
	"", 0, -1 };

dbg_parser RDI_dp = {
	0, 0, change_rdi_register, display_rdi_help, 0, "RDI", 0, -1,
	"", 0, -1 };

dbg_parser RBP_dp = {
	0, 0, change_rbp_register, display_rbp_help, 0, "RBP", 0, -1,
	"", 0, -1 };

dbg_parser RSP_dp = {
	0, 0, change_rsp_register, display_rsp_help, 0, "RSP", 0, -1,
	"", 0, -1 };

dbg_parser RIP_dp = {
	0, 0, change_rip_register, display_rip_help, 0, "RIP", 0, -1,
	"", 0, -1 };

dbg_parser R8_dp = {
	0, 0, change_r8_register, display_r8_help, 0, "R8", 0, -1,
	"", 0, -1 };

dbg_parser R9_dp = {
	0, 0, change_r9_register, display_r9_help, 0, "R9", 0, -1,
	"", 0, -1 };

dbg_parser R10_dp = {
	0, 0, change_r10_register, display_r10_help, 0, "R10", 0, -1,
	"", 0, -1 };

dbg_parser R11_dp = {
	0, 0, change_r11_register, display_r11_help, 0, "R11", 0, -1,
	"", 0, -1 };

dbg_parser R12_dp = {
	0, 0, change_r12_register, display_r12_help, 0, "R12", 0, -1,
	"", 0, -1 };

dbg_parser R13_dp = {
	0, 0, change_r13_register, display_r13_help, 0, "R13", 0, -1,
	"", 0, -1 };

dbg_parser R14_dp = {
	0, 0, change_r14_register, display_r14_help, 0, "R14", 0, -1,
	"", 0, -1 };

dbg_parser R15_dp = {
	0, 0, change_r15_register, display_r15_help, 0, "R15", 0, -1,
	"", 0, -1 };

#endif

dbg_parser CS_dp = {
	0, 0, change_cs_register, display_cs_help, 0, "XCS", 0, -1,
	"", 0, -1 };

#if !IS_ENABLED(CONFIG_X86_64)
dbg_parser DS_dp = {
	0, 0, change_ds_register, display_ds_help, 0, "XDS", 0, -1,
	"", 0, -1 };

dbg_parser ES_dp = {
	0, 0, change_es_register, display_es_help, 0, "XES", 0, -1,
	"", 0, -1 };
#endif

dbg_parser FS_dp = {
	0, 0, change_fs_register, display_fs_help, 0, "XFS", 0, -1,
	"", 0, -1 };

dbg_parser GS_dp = {
	0, 0, change_gs_register, display_gs_help, 0, "XGS", 0, -1,
	"", 0, -1 };

dbg_parser SS_dp = {
	0, 0, change_ss_register, display_ss_help, 0, "XSS", 0, -1,
	"", 0, -1 };

dbg_parser RF_dp = {
	0, 0, change_rf_flag, display_rf_help, 0, "RF", 0, -1,
	"", 0, -1 };

dbg_parser TF_dp = {
	0, 0, change_tf_flag, display_tf_help, 0, "TF", 0, -1,
	"", 0, -1 };

dbg_parser ZF_dp = {
	0, 0, change_zf_flag, display_zf_help, 0, "ZF", 0, -1,
	"", 0, -1 };

dbg_parser SF_dp = {
	0, 0, change_sf_flag, display_sf_help, 0, "SF", 0, -1,
	"", 0, -1 };

dbg_parser PF_dp = {
	0, 0, change_pf_flag, display_pf_help, 0, "PF", 0, -1,
	"", 0, -1 };

dbg_parser CF_dp = {
	0, 0, change_cf_flag, display_cf_help, 0, "CF", 0, -1,
	"", 0, -1 };

dbg_parser OF_dp = {
	0, 0, change_of_flag, display_of_help, 0, "OF", 0, -1,
	"", 0, -1 };

dbg_parser IF_dp = {
	0, 0, change_if_flag, display_if_help, 0, "IF", 0, -1,
	"", 0, -1 };

dbg_parser ID_dp = {
	0, 0, change_id_flag, display_id_help, 0, "CPUID", 0, -1,
	"", 0, -1 };

dbg_parser DF_dp = {
	0, 0, change_df_flag, display_df_help, 0, "DF", 0, -1,
	"", 0, -1 };

dbg_parser NT_dp = {
	0, 0, change_nt_flag, display_nt_help, 0, "NT", 0, -1,
	"", 0, -1 };

dbg_parser VM_dp = {
	0, 0, change_vm_flag, display_vm_help, 0, "VM", 0, -1,
	"", 0, -1 };

dbg_parser VIF_dp = {
	0, 0, change_vif_flag, display_vif_help, 0, "VIF", 0, -1,
	"", 0, -1 };

dbg_parser VIP_dp = {
	0, 0, change_vip_flag, display_vip_help, 0, "VIP", 0, -1,
	"", 0, -1 };

dbg_parser AF_dp = {
	0, 0, change_af_flag, display_af_help, 0, "AF", 0, -1,
	"", 0, -1 };

dbg_parser AC_dp = {
	0, 0, change_ac_flag, display_ac_help, 0, "AC", 0, -1,
	"", 0, -1 };

dbg_parser MTRR_dp = {
	0, 0, show_mtrr_registers, display_mtrr_help, 0, "MTRR", 0, 0,
	"display memory type range registers", 0, 8 };

#if IS_ENABLED(CONFIG_SMP) && !IS_ENABLED(CONFIG_X86_ELAN)
dbg_parser APIC__dp = {
	0, 0, display_apic_info, display_apic_help, 0, "APIC", 0, 0,
	"display local apic registers", 0, 7 };

dbg_parser IOAPIC__dp = {
	0, 0, display_ioapic_info, display_ioapic_help, 0, "IOAPIC", 0, 0,
	"display io apic registers", 0, 7 };

dbg_parser PERCPU__dp = {
	0, 0, dump_per_cpu, percpu_help, 0, "PERCPU", 0, 0,
	"display per cpu data and symbols", 0, 7 };
#endif

dbg_parser GDT_dp = {
	0, 0, show_gdt, display_gdt_help, 0, ".G", 0, 0,
	"display global descriptor table", 0, 8 };

dbg_parser IDT_dp = {
	0, 0, show_idt, display_idt_help, 0, ".I", 0, 0,
	"display interrupt descriptor table", 0, 8 };

dbg_parser eval_dp = {
	0, 0, evaluate_expr, evaluate_expr_help, 0, ".E", 0, 0,
	"evaluate expression (help .e)", 0, 6 };

dbg_parser input_word_dp = {
	0, 0, input_word_port, port_command_help, 0, "IW", 0, 0,
	"input word from port", 0, 8 };

dbg_parser input_double_dp = {
	0, 0, input_double_port, port_command_help, 0, "IL", 0, 0,
	"input double word from port", 0, 8 };

dbg_parser input_byte_dp = {
	0, 0, input_byte_port, port_command_help, 0, "IB", 0, 0,
	"input byte from port", 0, 8 };

dbg_parser input_dp = {
	0, 0, input_port, port_command_help, 0, "I", 0, 0,
	"input byte from port", 0, 8 };

dbg_parser output_word_dp = {
	0, 0, output_word_port, port_command_help, 0, "OW", 0, 0,
	"output word to port", 0, 8 };

dbg_parser output_double_dp = {
	0, 0, output_double_port, port_command_help, 0, "OL", 0, 0,
	"output double word to port", 0, 8 };

dbg_parser output_byte_dp = {
	0, 0, output_byte_port, port_command_help, 0, "OB", 0, 0,
	"output byte to port", 0, 8 };

dbg_parser output_dp = {
	0, 0, output_port, port_command_help, 0, "O", 0, 0,
	"output byte to port", 0, 8 };

dbg_parser break_clear_all_dp = {
	0, 0, breakpoint_clear_all, breakpoint_command_help, 0, "BCA", 0, 0,
	"clear all breakpoints", 0, 3 };

dbg_parser break_clear_dp = {
	0, 0, breakpoint_clear, breakpoint_command_help, 0, "BC", 0, 0,
	"clear breakpoint", 0, 3 };

dbg_parser break_mask_dp = {
	0, 0, breakpoint_mask, breakpoint_command_help, 0, "BM", 0, 0,
	"mask breaks for specific processor", 0, 3 };

dbg_parser BW1_dp = {
	0, 0, breakpoint_word1, breakpoint_command_help, 0, "BW1", 0, -1,
	"", 0, 3 };

dbg_parser BW2_dp = {
	0, 0, breakpoint_word2, breakpoint_command_help, 0, "BW2", 0, -1,
	"", 0, 3 };

dbg_parser BW4_dp = {
	0, 0, breakpoint_word4, breakpoint_command_help, 0, "BW4", 0, -1,
	"", 0, 3 };

#if IS_ENABLED(CONFIG_X86_64)
dbg_parser BW8_dp = {
	0, 0, breakpoint_word8, breakpoint_command_help, 0, "BW8", 0, -1,
	"", 0, 3 };
#endif

dbg_parser BW_dp = {
	0, 0, breakpoint_word, breakpoint_command_help, 0, "BW", 0, 0,
	"set write only breakpoint #=1, 2, 4 or 8 byte len", 0, 3 };

dbg_parser BR1_dp = {
	0, 0, breakpoint_read1, breakpoint_command_help, 0, "BR1", 0, -1,
	"", 0, 3 };

dbg_parser BR2_dp = {
	0, 0, breakpoint_read2, breakpoint_command_help, 0, "BR2", 0, -1,
	"", 0, 3 };

dbg_parser BR4_dp = {
	0, 0, breakpoint_read4, breakpoint_command_help, 0, "BR4", 0, -1,
	"", 0, 3 };

#if IS_ENABLED(CONFIG_X86_64)
dbg_parser BR8_dp = {
	0, 0, breakpoint_read8, breakpoint_command_help, 0, "BR8", 0, -1,
	"", 0, 3 };
#endif

dbg_parser BR_dp = {
	0, 0, breakpoint_read, breakpoint_command_help, 0, "BR", 0, 0,
	"set read/write breakpoint #=1, 2, 4 or 8 byte len", 0, 3 };

dbg_parser BI1_dp = {
	0, 0, breakpoint_io1, breakpoint_command_help, 0, "BI1", 0, -1,
	"", 0, 3 };

dbg_parser BI2_dp = {
	0, 0, breakpoint_io2, breakpoint_command_help, 0, "BI2", 0, -1,
	"", 0, 3 };

dbg_parser BI4_dp = {
	0, 0, breakpoint_io4, breakpoint_command_help, 0, "BI4", 0, -1,
	"", 0, 3 };

dbg_parser BI_dp = {
	0, 0, breakpoint_io, breakpoint_command_help, 0, "BI", 0, 0,
	"set io address breakpoint #=1, 2 or 4 byte len", 0, 3 };

dbg_parser breakpoint_execute_dp = {
	0, 0, breakpoint_execute, breakpoint_command_help, 0, "B", 0, 0,
	"display all/set execute breakpoint", 0, 3 };

dbg_parser break_show_temp = {
	0, 0, breakpoint_show_temp, breakpoint_command_help, 0, "BST", 0, 0,
	"displays temporary breakpoints (proceed/go)", 0, 3 };

dbg_parser break_timer = {
	0, 0, timer_breakpoint, timer_break_help, 0, "ADDTIMER", 0, 0,
	"add a debug timer event", 0, 3 };

dbg_parser break_timer_clear = {
	0, 0, timer_del_break, timer_break_help, 0, "DELTIMER", 0, 0,
	"delete a debug timer event", 0, 3 };

/* interactive debugger accelerators */

accel_key trace_ssb_acc = {
	0, 0, process_trace_ssb_acc, 0, 0, K_F6, 0,
	"F6 - Trace/Single Step til Branch" };

accel_key trace_acc = {
	0, 0, process_trace_acc, 0, 0, K_F7, 0,
	"F7 - Trace/Single Step" };

accel_key proceed_acc = {
	0, 0, process_proceed_acc, 0, 0, K_F8, 0,
	"F8 - Proceed" };

accel_key go_acc = {
	0, 0, process_go_acc, 0, 0, K_F9, 0,
	"F9 - Go" };

accel_key enter_acc = { /* this accelerator handles repeat command */
	0, 0, enter_key_acc, 0, 0, 13, 0,   /* processing */
	"Enter - Execute or Repeat a Command" };

unsigned char *last_dump;
unsigned char *last_link;
unsigned long last_unasm;
unsigned long display_length;
unsigned long last_cmd;
unsigned long last_cmd_key;
unsigned char last_debug_command[100] = {""};
unsigned long last_display;
unsigned char debug_command[100] = {""};
unsigned long next_unasm;
unsigned long pic_1_value;
unsigned long pic_2_value;
unsigned long break_enabled[4];
unsigned long break_reserved[4];
unsigned long break_points[4];
unsigned long break_type[4];
unsigned long break_length[4];
unsigned long break_temp[4];
unsigned long break_go[4];
unsigned long break_proceed[4];
unsigned long conditional_breakpoint[4];
unsigned char break_condition[4][256];
dbg_regs last_dbg_regs;
unsigned long last_cr0;
unsigned long last_cr2;
unsigned long last_cr4;
unsigned long current_dr7;
unsigned long repeat_command;
unsigned long total_lines;
unsigned long debugger_initialized;
unsigned long ssbmode;

DEFINE_PER_CPU(unsigned long, current_dr6);
DEFINE_PER_CPU(unsigned long, break_mask);
DEFINE_PER_CPU(unsigned long, processor_hold);
DEFINE_PER_CPU(unsigned long, processor_state);
DEFINE_PER_CPU(atomic_t, debugger_processors);
DEFINE_PER_CPU(atomic_t, nmi_processors);
DEFINE_PER_CPU(atomic_t, trace_processors);
DEFINE_PER_CPU(dbg_regs, current_dbg_regs);

void mdb_breakpoint(void)
{
	asm volatile("int3");
}

void initialize_debugger(void)
{
	register unsigned long i;

	last_cmd = 0;
	last_cmd_key = 0;
	last_display = 0;

	for (i = 0; i < 4; i++) {
		break_reserved[i] = 0;
		break_points[i] = 0;
		break_type[i] = 0;
		break_length[i] = 0;
		break_proceed[i] = 0;
		break_go[i] = 0;
		break_temp[i] = 0;
		conditional_breakpoint[i] = 0;
		break_condition[i][0] = '\0';
	}

	initialize_debugger_registers();

	add_debug_parser(&reason_dp);
	add_debug_parser(&back_trace_all_pid_dp);
	add_debug_parser(&back_trace_pid_dp);
	add_debug_parser(&back_trace_stack_dp);
	add_debug_parser(&cpu_frame_dp);
	add_debug_parser(&processor_dp);
	add_debug_parser(&sh_sp);
	add_debug_parser(&help_dp);
	add_debug_parser(&ascii_table_dp);
	add_debug_parser(&display_toggle_1);
	add_debug_parser(&display_toggle_2);
	add_debug_parser(&TB_toggle_dp);
	add_debug_parser(&TU_toggle_dp);
	add_debug_parser(&TD_toggle_dp);
	add_debug_parser(&TL_toggle_dp);
	add_debug_parser(&TG_toggle_dp);
	add_debug_parser(&TC_toggle_dp);
	add_debug_parser(&TN_toggle_dp);
	add_debug_parser(&TR_toggle_dp);
	add_debug_parser(&TS_toggle_dp);
	add_debug_parser(&TA_toggle_dp);
	add_debug_parser(&toggle_user);
	add_debug_parser(&T_toggle_dp);
	add_debug_parser(&version_dp);
	add_debug_parser(&reboot_dp);
	add_debug_parser(&kprocess_1);
	add_debug_parser(&kprocess_2);
	add_debug_parser(&all_symbols_dp);
	add_debug_parser(&symbols_dp);
#if IS_ENABLED(CONFIG_MODULES)
	add_debug_parser(&lsmod_pe_1);
	add_debug_parser(&lsmod_pe_2);
	add_debug_parser(&rmmod_dp);
#endif
	add_debug_parser(&control_dp);
	add_debug_parser(&all_dp);
	add_debug_parser(&segment_dp);
	add_debug_parser(&numeric_dp);
	add_debug_parser(&general_dp);
	add_debug_parser(&default_dp);
	add_debug_parser(&search_memory_b_dp);
	add_debug_parser(&search_memory_w_dp);
	add_debug_parser(&search_memory_d_dp);
	add_debug_parser(&search_memory_q_dp);
	add_debug_parser(&change_word_dp);
	add_debug_parser(&change_double_dp);
	add_debug_parser(&change_quad_dp);
	add_debug_parser(&change_byte_dp);
	add_debug_parser(&change_default_dp);
	add_debug_parser(&close_symbols_dp);
	add_debug_parser(&walk_dp);
	add_debug_parser(&dump_linked_dp);
	add_debug_parser(&dump_word_dp);
	add_debug_parser(&dump_stack_dp);
	add_debug_parser(&dump_double_stack_dp);
	add_debug_parser(&dump_double_dp);
	add_debug_parser(&dump_quad_stack_dp);
	add_debug_parser(&dump_quad_dp);
	add_debug_parser(&dump_byte_dp);
	add_debug_parser(&dump_default_dp);
	add_debug_parser(&dump_byte_phys_dp);
	add_debug_parser(&dump_quad_phys_dp);
	add_debug_parser(&dump_double_phys_dp);
	add_debug_parser(&dump_word_phys_dp);
	add_debug_parser(&dump_default_phys_dp);
	add_debug_parser(&diss_16_dp);
	add_debug_parser(&diss_32_dp);
	add_debug_parser(&diss_any_dp);
	add_debug_parser(&id_dp);
	add_debug_parser(&proceed_dp);
	add_debug_parser(&trace_dp);
	add_debug_parser(&single_step_dp);
	add_debug_parser(&trace_ss_dp);
	add_debug_parser(&trace_ssb_dp);
	add_debug_parser(&G_dp);
	add_debug_parser(&go_dp);
	add_debug_parser(&Q_dp);
	add_debug_parser(&X_dp);
	add_debug_parser(&break_processor_dp);
	add_debug_parser(&list_processors_dp);
#if IS_ENABLED(CONFIG_X86_64)
	add_debug_parser(&RAX_dp);
	add_debug_parser(&ORIGRAX_dp);
	add_debug_parser(&RBX_dp);
	add_debug_parser(&RCX_dp);
	add_debug_parser(&RDX_dp);
	add_debug_parser(&RSI_dp);
	add_debug_parser(&RDI_dp);
	add_debug_parser(&RBP_dp);
	add_debug_parser(&RSP_dp);
	add_debug_parser(&RIP_dp);
	add_debug_parser(&R8_dp);
	add_debug_parser(&R9_dp);
	add_debug_parser(&R10_dp);
	add_debug_parser(&R11_dp);
	add_debug_parser(&R12_dp);
	add_debug_parser(&R13_dp);
	add_debug_parser(&R14_dp);
	add_debug_parser(&R15_dp);
#endif
	add_debug_parser(&ORIGEAX_dp);
	add_debug_parser(&AL_dp);
	add_debug_parser(&BL_dp);
	add_debug_parser(&CL_dp);
	add_debug_parser(&DL_dp);
	add_debug_parser(&AX_dp);
	add_debug_parser(&BX_dp);
	add_debug_parser(&CX_dp);
	add_debug_parser(&DX_dp);
	add_debug_parser(&EAX_dp);
	add_debug_parser(&EBX_dp);
	add_debug_parser(&ECX_dp);
	add_debug_parser(&EDX_dp);
	add_debug_parser(&ESI_dp);
	add_debug_parser(&EDI_dp);
	add_debug_parser(&EBP_dp);
	add_debug_parser(&ESP_dp);
	add_debug_parser(&EIP_dp);
	add_debug_parser(&CS_dp);
#if !IS_ENABLED(CONFIG_X86_64)
	add_debug_parser(&DS_dp);
	add_debug_parser(&ES_dp);
#endif
	add_debug_parser(&FS_dp);
	add_debug_parser(&GS_dp);
	add_debug_parser(&SS_dp);
	add_debug_parser(&RF_dp);
	add_debug_parser(&TF_dp);
	add_debug_parser(&ZF_dp);
	add_debug_parser(&SF_dp);
	add_debug_parser(&PF_dp);
	add_debug_parser(&CF_dp);
	add_debug_parser(&OF_dp);
	add_debug_parser(&IF_dp);
	add_debug_parser(&ID_dp);
	add_debug_parser(&DF_dp);
	add_debug_parser(&NT_dp);
	add_debug_parser(&VM_dp);
	add_debug_parser(&VIF_dp);
	add_debug_parser(&VIP_dp);
	add_debug_parser(&AF_dp);
	add_debug_parser(&AC_dp);
	add_debug_parser(&MTRR_dp);
#if IS_ENABLED(CONFIG_SMP) && !IS_ENABLED(CONFIG_X86_ELAN)
	add_debug_parser(&APIC__dp);
	add_debug_parser(&IOAPIC__dp);
	add_debug_parser(&PERCPU__dp);
#endif
	add_debug_parser(&GDT_dp);
	add_debug_parser(&IDT_dp);
	add_debug_parser(&eval_dp);
	add_debug_parser(&input_word_dp);
	add_debug_parser(&input_double_dp);
	add_debug_parser(&input_byte_dp);
	add_debug_parser(&input_dp);
	add_debug_parser(&output_word_dp);
	add_debug_parser(&output_double_dp);
	add_debug_parser(&output_byte_dp);
	add_debug_parser(&output_dp);
	add_debug_parser(&break_clear_all_dp);
	add_debug_parser(&break_clear_dp);
	add_debug_parser(&break_mask_dp);
	add_debug_parser(&BW1_dp);
	add_debug_parser(&BW2_dp);
	add_debug_parser(&BW4_dp);
#if IS_ENABLED(CONFIG_X86_64)
	add_debug_parser(&BW8_dp);
#endif
	add_debug_parser(&BW_dp);
	add_debug_parser(&BR1_dp);
	add_debug_parser(&BR2_dp);
	add_debug_parser(&BR4_dp);
#if IS_ENABLED(CONFIG_X86_64)
	add_debug_parser(&BR8_dp);
#endif
	add_debug_parser(&BR_dp);
	add_debug_parser(&BI1_dp);
	add_debug_parser(&BI2_dp);
	add_debug_parser(&BI4_dp);
	add_debug_parser(&BI_dp);
	add_debug_parser(&breakpoint_execute_dp);
	add_debug_parser(&break_show_temp);
	add_debug_parser(&break_timer);
	add_debug_parser(&break_timer_clear);

	add_accel_routine(&trace_ssb_acc);
	add_accel_routine(&trace_acc);
	add_accel_routine(&proceed_acc);
	add_accel_routine(&go_acc);
	add_accel_routine(&enter_acc);

	debugger_initialized = 1;
}

void clear_debugger_state(void)
{
	clear_debugger_registers();
	debugger_initialized = 0;
}

void clear_temp_breakpoints(void)
{
	register unsigned long i;

	for (i = 0; i < 4; i++) {
		if (break_temp[i]) {
			break_temp[i] = 0;
			break_reserved[i] = 0;
			break_points[i] = 0;
			break_type[i] = 0;
			break_length[i] = 0;
			break_go[i] = 0;
			break_proceed[i] = 0;
		}
	}
	set_debug_registers();
}

unsigned long valid_breakpoint(unsigned long address)
{
	register unsigned long i;

	for (i = 0; i < 4; i++) {
		if (!break_temp[i])
			if (break_points[i] == address)
				return 1;
	}
	return 0;
}

unsigned long get_ip(dbg_regs *dbgframe)
{
	return (unsigned long)(dbgframe->t_ip);
}

unsigned long get_stack_address(dbg_regs *dbgframe)
{
	return (unsigned long)(dbgframe->t_sp);
}

unsigned long get_stack_segment(dbg_regs *dbgframe)
{
	return (unsigned long)(dbgframe->t_ss);
}

unsigned long ssb_update(dbg_regs *dbgframe, unsigned long processor)
{
	if (!ssbmode)
		return 0;

	if (jmp_active) {
		ssbmode = 0;
		return 0;
	}

	last_cmd = 'T';
	strcpy(debug_command, "SSB");
	last_cr0 = dbg_read_cr0();
	last_cr2 = dbg_read_cr2();
	last_cr4 = dbg_read_cr4();
	memmove((void *)&last_dbg_regs, dbgframe,
		sizeof(dbg_regs));

	dbgframe->t_flags |= (SINGLE_STEP | RESUME);

	atomic_inc(&focus_active);
	atomic_inc(&per_cpu(trace_processors, get_processor_id()));
	return -1;
}

/* F6 */

unsigned long process_trace_ssb_acc(unsigned long key, void *p,
				    accel_key *accel)
{
	register dbg_regs *dbgframe = p;

	dbg_pr("\n");
	ssbmode = 1;
	last_cmd = 'T';
	strcpy(debug_command, "SSB");
	last_cr0 = dbg_read_cr0();
	last_cr2 = dbg_read_cr2();
	last_cr4 = dbg_read_cr4();
	memmove((void *)&last_dbg_regs, dbgframe, sizeof(dbg_regs));

	dbgframe->t_flags |= (SINGLE_STEP | RESUME);

	/* set as focus processor for trace, ssb, or proceed */
	atomic_inc(&focus_active);
	atomic_inc(&per_cpu(trace_processors, get_processor_id()));
	return -1;
}

/* F8 */

unsigned long process_proceed_acc(unsigned long key, void *p,
				  accel_key *accel)
{
	register dbg_regs *dbgframe = p;
	register unsigned long i;

	ssbmode = 0;
	dbg_pr("\n");
	if (needs_proceed) {
		for (i = 0; i < 4; i++) {
			if (!break_reserved[i]) {
				break_reserved[i] = 1;
				break_points[i] = next_unasm;
				break_type[i] = BREAK_EXECUTE;
				break_length[i] = ONE_BYTE_FIELD;
				break_temp[i] = 1;
				break_proceed[i] = 1;
				set_debug_registers();
				last_cmd = 'P';
				strcpy(debug_command, "P");
				last_cr0 = dbg_read_cr0();
				last_cr2 = dbg_read_cr2();
				last_cr4 = dbg_read_cr4();
				memmove((void *)&last_dbg_regs, dbgframe,
					sizeof(dbg_regs));

				dbgframe->t_flags &= ~SINGLE_STEP;
				dbgframe->t_flags |= RESUME;

				/* set as focus processor */
				atomic_inc(&focus_active);
				atomic_inc(&per_cpu(trace_processors,
						    get_processor_id()));
				return -1;
			}
		}
		dbg_pr("\nNo breakpoint for Proceed, single step instead");
	}
	last_cmd = 'P';
	strcpy(debug_command, "P");
	last_cr0 = dbg_read_cr0();
	last_cr2 = dbg_read_cr2();
	last_cr4 = dbg_read_cr4();
	memmove((void *)&last_dbg_regs, dbgframe,
		sizeof(dbg_regs));

	dbgframe->t_flags |= (SINGLE_STEP | RESUME);

	/* set as focus processor for trace, ssb, or proceed */
	atomic_inc(&focus_active);
	atomic_inc(&per_cpu(trace_processors, get_processor_id()));
	return -1;
}

/* F7 */

unsigned long process_trace_acc(unsigned long key, void *p, accel_key *accel)
{
	register dbg_regs *dbgframe = p;

	ssbmode = 0;
	dbg_pr("\n");
	last_cmd = 'T';
	strcpy(debug_command, "T");
	last_cr0 = dbg_read_cr0();
	last_cr2 = dbg_read_cr2();
	last_cr4 = dbg_read_cr4();
	memmove((void *)&last_dbg_regs, dbgframe,
		sizeof(dbg_regs));

	dbgframe->t_flags |= (SINGLE_STEP | RESUME);

	/* set as focus processor for trace, ssb, or proceed */
	atomic_inc(&focus_active);
	atomic_inc(&per_cpu(trace_processors, get_processor_id()));
	return -1;
}

/* F9 */

unsigned long process_go_acc(unsigned long key, void *p, accel_key *accel)
{
	register dbg_regs *dbgframe = p;

	ssbmode = 0;
	dbg_pr("\n");
	clear_temp_breakpoints();
	last_cmd = 'G';
	last_cr0 = dbg_read_cr0();
	last_cr2 = dbg_read_cr2();
	last_cr4 = dbg_read_cr4();
	memmove((void *)&last_dbg_regs, dbgframe, sizeof(dbg_regs));

	dbgframe->t_flags &= ~SINGLE_STEP;
	dbgframe->t_flags |= RESUME;
	return -1;
}

unsigned long execute_command_help(unsigned char *command_line,
				   dbg_parser *parser)
{
	dbg_pr("t                        - trace\n");
	dbg_pr("s                        - single step\n");
	dbg_pr("ss                       - single step\n");
	dbg_pr("ssb                      - single step til branch\n");
	dbg_pr("p                        - proceed\n");
	dbg_pr("g or g <address>         - go\n");
	dbg_pr("go or go <address>       - go\n");
	dbg_pr("q or q <address>         - quit\n");
	dbg_pr("x or x <address>         - exit\n");
	dbg_pr("F7                       - trace\n");
	dbg_pr("F8                       - proceed\n");
	dbg_pr("F9                       - go\n");
	dbg_pr("\n");
	return 1;
}

/* P */

unsigned long process_proceed(unsigned char *cmd,
			      dbg_regs *dbgframe, unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned long i;

	ssbmode = 0;
	if (needs_proceed) {
		for (i = 0; i < 4; i++) {
			if (!break_reserved[i]) {
				break_reserved[i] = 1;
				break_points[i] = next_unasm;
				break_type[i] = BREAK_EXECUTE;
				break_length[i] = ONE_BYTE_FIELD;
				break_temp[i] = 1;
				break_proceed[i] = 1;
				set_debug_registers();
				last_cmd = 'P';
				last_cr0 = dbg_read_cr0();
				last_cr2 = dbg_read_cr2();
				last_cr4 = dbg_read_cr4();
				memmove((void *)&last_dbg_regs, dbgframe,
					sizeof(dbg_regs));

				dbgframe->t_flags &= ~SINGLE_STEP;
				dbgframe->t_flags |= RESUME;

				/* set as focus processor */
				atomic_inc(&focus_active);
				atomic_inc(&per_cpu(trace_processors,
						    get_processor_id()));
				return -1;
			}
		}
		dbg_pr("\nNo breakpoint for Proceed, single step instead");
	}
	last_cmd = 'P';
	last_cr0 = dbg_read_cr0();
	last_cr2 = dbg_read_cr2();
	last_cr4 = dbg_read_cr4();
	memmove((void *)&last_dbg_regs, dbgframe,
		sizeof(dbg_regs));

	dbgframe->t_flags |= (SINGLE_STEP | RESUME);

	/* set as focus processor for trace, ssb, or proceed */
	atomic_inc(&focus_active);
	atomic_inc(&per_cpu(trace_processors, get_processor_id()));
	return -1;
}

/* SSB */

unsigned long process_trace_ssb(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	dbg_pr("\n");
	ssbmode = 1;
	last_cmd = 'T';
	last_cr0 = dbg_read_cr0();
	last_cr2 = dbg_read_cr2();
	last_cr4 = dbg_read_cr4();
	memmove((void *)&last_dbg_regs, dbgframe,
		sizeof(dbg_regs));

	dbgframe->t_flags |= (SINGLE_STEP | RESUME);

	/* set as focus processor for trace, ssb, or proceed */
	atomic_inc(&focus_active);
	atomic_inc(&per_cpu(trace_processors, get_processor_id()));
	return -1;
}

/* T */

unsigned long process_trace(unsigned char *cmd,
			    dbg_regs *dbgframe, unsigned long exception,
			    dbg_parser *parser)
{
	ssbmode = 0;
	last_cmd = 'T';
	last_cr0 = dbg_read_cr0();
	last_cr2 = dbg_read_cr2();
	last_cr4 = dbg_read_cr4();
	memmove((void *)&last_dbg_regs, dbgframe,
		sizeof(dbg_regs));

	dbgframe->t_flags |= (SINGLE_STEP | RESUME);

	/* set as focus processor for trace, ssb, or proceed */
	atomic_inc(&focus_active);
	atomic_inc(&per_cpu(trace_processors, get_processor_id()));
	return -1;
}

/* G */

unsigned long process_go(unsigned char *cmd,
			 dbg_regs *dbgframe, unsigned long exception,
			 dbg_parser *parser)
{
	register unsigned long address;
	unsigned long valid;
	register unsigned long i;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	ssbmode = 0;
	clear_temp_breakpoints();
	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		for (i = 0; i < 4; i++) {
			if (!break_reserved[i]) {
				break_reserved[i] = 1;
				break_points[i] = address;
				break_type[i] = BREAK_EXECUTE;
				break_length[i] = ONE_BYTE_FIELD;
				break_temp[i] = 1;
				break_go[i] = 1;
				set_debug_registers();
				dbg_pr("\n");
				last_cmd = 'G';
				last_cr0 = dbg_read_cr0();
				last_cr2 = dbg_read_cr2();
				last_cr4 = dbg_read_cr4();
				memmove((void *)&last_dbg_regs, dbgframe,
					sizeof(dbg_regs));

				dbgframe->t_flags &= ~SINGLE_STEP;
				dbgframe->t_flags |= RESUME;
				return -1;
			}
		}
	} else {
		dbg_pr("\n");
		last_cmd = 'G';
		last_cr0 = dbg_read_cr0();
		last_cr2 = dbg_read_cr2();
		last_cr4 = dbg_read_cr4();
		memmove((void *)&last_dbg_regs, dbgframe,
			sizeof(dbg_regs));

		dbgframe->t_flags &= ~SINGLE_STEP;
		dbgframe->t_flags |= RESUME;
		return -1;
	}
	dbg_pr("no breakpoint available for GO\n");
	return 1;
}

unsigned long processor_command_help(unsigned char *command_line,
				     dbg_parser *parser)
{
	dbg_pr("lcpu                     - list processors\n");
	dbg_pr("cpu[p#]                 - switch processor\n");
	dbg_pr("lr[p#]                 - display processor registers\n");
	return 1;
}

/* CPU */

unsigned long break_processor(unsigned char *cmd,
			      dbg_regs *dbgframe, unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned long cpunum, cpu = get_processor_id();
	unsigned long valid, i;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	cpunum = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		if (cpunum == cpu) {
			dbg_pr("debugger already running on processor %d\n",
			       (int)cpunum);
			return 1;
		}

		if ((cpunum > MAX_PROCESSORS) || !(cpu_online(cpunum))) {
			dbg_pr("invalid processor specified\n");
			return 1;
		}

		for (i = 0; i < MAX_PROCESSORS; i++) {
			if (cpu_online(i)) {
				if (i == cpunum) {
					per_cpu(processor_state, i) =
						PROCESSOR_SWITCH;
					/* serialize processors */
					smp_mb__before_atomic();
					per_cpu(processor_hold, cpu) = 1;
					/* serialize processors */
					smp_mb__after_atomic();
					break;
				}
			}
		}
		dbg_pr("\n");
		last_cmd = 'G';
		last_cr0 = dbg_read_cr0();
		last_cr2 = dbg_read_cr2();
		last_cr4 = dbg_read_cr4();
		memmove((void *)&last_dbg_regs, dbgframe,
			sizeof(dbg_regs));
		return -1;
	}
	dbg_pr("no target processor specified\n");
	dbg_pr("Current Processor: %d\n", get_processor_id());
	dbg_pr("Active Processors: ");

	for (i = 0; i < MAX_PROCESSORS; i++) {
		if (cpu_online(i)) {
			if (i)
				dbg_pr(", ");
			dbg_pr("%d", i);
		}
	}
	dbg_pr("\n");
	return 1;
}

#if IS_ENABLED(CONFIG_SMP) && !IS_ENABLED(CONFIG_X86_ELAN)
/* NMI */

unsigned long nmi_processor(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser)
{
	register unsigned long cpunum, cpu = get_processor_id();
	unsigned long valid, i;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	cpunum = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		if (cpunum == cpu) {
			dbg_pr("debugger already running on processor %d\n",
			       (int)cpunum);
			return 1;
		}

		if ((cpunum > MAX_PROCESSORS) || !(cpu_online(cpunum))) {
			dbg_pr("invalid processor specified\n");
			return 1;
		}

		for (i = 0; i < MAX_PROCESSORS; i++) {
			if (cpu_online(i)) {
				if (i == cpunum) {
					per_cpu(processor_state, i) =
						PROCESSOR_SWITCH;
					/* serialize processors */
					smp_mb__before_atomic();
					per_cpu(processor_hold, cpu) = 1;
					/* serialize processors */
					smp_mb__after_atomic();
					break;
				}
			}
		}
		dbg_pr("\n");
		last_cmd = 'G';
		last_cr0 = dbg_read_cr0();
		last_cr2 = dbg_read_cr2();
		last_cr4 = dbg_read_cr4();
		memmove((void *)&last_dbg_regs, dbgframe,
			sizeof(dbg_regs));
		return -1;
	}
	dbg_pr("no target processor specified\n");
	dbg_pr("Current Processor: %d\n", get_processor_id());
	dbg_pr("Active Processors: ");

	for (i = 0; i < MAX_PROCESSORS; i++) {
		if (cpu_online(i)) {
			if (i)
				dbg_pr(", ");
			dbg_pr("%d", i);
		}
	}
	dbg_pr("\n");
	return 1;
}
#endif

/* LR */

unsigned long list_processor_frame(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser)
{
	unsigned long valid, pnum;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	pnum = eval_expr(dbgframe, &cmd, &valid);
	if (valid && (pnum < MAX_PROCESSORS) && (cpu_online(pnum))) {
		dbg_regs *list_frame =
			(dbg_regs *)&per_cpu(current_dbg_regs, pnum);

		dbg_pr("Processor Frame %d -> (%lX)\n", pnum,
		       &per_cpu(current_dbg_regs, pnum));
		display_tss(list_frame);
		closest_symbol(list_frame->t_ip);
		bt_stack(NULL, NULL, (unsigned long *)list_frame->t_sp);
	} else {
		dbg_pr("invalid processor frame\n");
	}

	return 1;
}

/* .TA */

unsigned long process_ta_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	if (general_toggle) {
		general_toggle = 0;
		control_toggle = 0;
		segment_toggle = 0;
	} else {
		general_toggle = 1;
		control_toggle = 1;
		segment_toggle = 1;
	}

	dbg_pr("general registers (%s)\n", general_toggle ? "ON" : "OFF");
	dbg_pr("control registers (%s)\n", control_toggle ? "ON" : "OFF");
	dbg_pr("segment registers (%s)\n", segment_toggle ? "ON" : "OFF");
	return 1;
}

unsigned long tss_display_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	dbg_pr(".t <address>             - display task state regs\n");
	return 1;
}

/* .T */

unsigned long tss_display(unsigned char *cmd,
			  dbg_regs *dbgframe,
			  unsigned long exception,
			  dbg_parser *parser)
{
	register unsigned long address;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid)
		display_tss(dbgframe);
	else
		display_tss((dbg_regs *)address);

	return 1;
}

unsigned long display_registers_help(unsigned char *command_line,
				     dbg_parser *parser)
{
	dbg_pr("r                        - display registers\n");
	dbg_pr("rc                       - display control registers\n");
	dbg_pr("rs                       - display segment registers\n");
	dbg_pr("rg                       - display general registers\n");
	dbg_pr("ra                       - display all registers\n");
	dbg_pr("rn                       - display coprocessor\n");

	return 1;
}

/* RC */

unsigned long control_registers(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	dbg_pr("Control Registers\n");
	display_control_registers(get_processor_id(), dbgframe);
	return 1;
}

/* RA */

unsigned long display_all_registers(unsigned char *cmd,
				    dbg_regs *dbgframe,
				    unsigned long exception,
				    dbg_parser *parser)
{
	register unsigned long processor = get_processor_id();

	dbg_pr("General Registers\n");
	display_general_registers(dbgframe);

	dbg_pr("Segment Registers\n");
	display_segment_registers(dbgframe);

	dbg_pr("Control Registers\n");
	display_control_registers(processor, dbgframe);

	if (fpu_present()) {
		dbg_pr("Coprocessor Registers\n");
		display_npx_registers(dbgframe);
	} else {
		dbg_pr("Coprocessor Not Present\n");
	}

	return 1;
}

/* RS */

unsigned long segment_registers(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	dbg_pr("Segment Registers\n");
	display_segment_registers(dbgframe);
	return 1;
}

/* RN */

unsigned long display_numeric_registers(unsigned char *cmd,
					dbg_regs *dbgframe,
					unsigned long exception,
					dbg_parser *parser)
{
	if (fpu_present()) {
		dbg_pr("Coprocessor Registers\n");
		display_npx_registers(dbgframe);
	} else {
		dbg_pr("Coprocessor Not Present\n");
	}
	return 1;
}

/* RG */

unsigned long general_registers(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	dbg_pr("General Registers\n");
	display_general_registers(dbgframe);
	return 1;
}

#if defined(RENDER_NPX_VALUES)
double ldexp(double v, int e)
{
	double two = 2.0;

	if (e < 0) {
		e = -e; /* This just might overflow. */
		if (e < 0)
			return 0.0;

		while (e > 0) {
			if (e & 1)
				v = v / two;
			two = two * two;
			e >>= 1;
		}
	} else if (e > 0) {
		while (e > 0) {
			if (e & 1)
				v = v * two;
			two = two * two;
			e >>= 1;
		}
	}
	return v;
}
#endif

void display_npx_registers(dbg_regs *dbgframe)
{
	NUMERIC_FRAME s_npx, *npx = &s_npx;
	register int i;
	int tag;
	int tos;
#if defined(RENDER_NPX_VALUES)
	double d;
#endif

	if (!fpu_present()) {
		dbg_pr("numeric co-processor not present\n");
		return;
	}

	save_npx(npx);

	tos = (npx->status >> 11) & 7;

	dbg_pr("Control: 0x%04X  Status: 0x%04X  ",
	       (unsigned int)npx->control & 0xFFFF,
	       (unsigned int)npx->status & 0xFFFF);

	dbg_pr("Tag: 0x%04X  TOS: %i CPU: %i\n",
	       (unsigned int)npx->tag & 0xFFFF,
	       (int)tos, (int)get_processor_id());

	for (i = 0; i < 8; i++) {
		tos = (npx->status >> 11) & 7;
		dbg_pr("st(%d)/MMX%d  ", i, (int)((tos + i) % 8));

		if (npx->reg[i].sign)
			dbg_pr("-");
		else
			dbg_pr("+");

		dbg_pr(" %04X %04X %04X %04X e %04X    ",
		       (unsigned int)npx->reg[i].sig3,
		       (unsigned int)npx->reg[i].sig2,
		       (unsigned int)npx->reg[i].sig1,
		       (unsigned int)npx->reg[i].sig0,
		       (unsigned int)npx->reg[i].exponent);

		tag = (npx->tag >> (((i + tos) % 8) * 2)) & 3;
		switch (tag) {
		case 0:
			dbg_pr("Valid");
#if defined(RENDER_NPX_VALUES)
			if (((int)npx->reg[i].exponent - 16382 < 1000) &&
			    ((int)npx->reg[i].exponent - 16382 > -1000)) {
				d = npx->reg[i].sig3 / 65536.0 +
					npx->reg[i].sig2 / 65536.0 /
					65536.0 + npx->reg[i].sig1 /
					65536.0 / 65536.0 / 65536.0;

				d = ldexp(d,
					  (int)npx->reg[i].exponent - 16382);

				if (npx->reg[i].sign)
					d = -d;

				dbg_pr("  %.16g", d);
			} else {
				dbg_pr("  (too big to display)");
			}
#endif
			dbg_pr("\n");
			break;

		case 1:
			dbg_pr("Zero\n");
			break;

		case 2:
			dbg_pr("Special\n");
			break;

		case 3:
			dbg_pr("Empty\n");
			break;
		}
	}
}

/* R */

unsigned long display_default_registers(unsigned char *cmd,
					dbg_regs *dbgframe,
					unsigned long exception,
					dbg_parser *parser)
{
	register unsigned long processor = get_processor_id();

	display_general_registers(dbgframe);

	if (control_toggle)
		display_control_registers(processor, dbgframe);

	if (numeric_toggle)
		display_npx_registers(dbgframe);

	disassemble(dbgframe, dbgframe->t_ip, 1, -1, 0);
	return 1;
}

void display_registers(dbg_regs *dbgframe, unsigned long processor)
{
	if (general_toggle)
		display_general_registers(dbgframe);

	if (control_toggle)
		display_control_registers(processor, dbgframe);

	if (numeric_toggle)
		display_npx_registers(dbgframe);
}

/*  */
/* IA32 Registers */
/*  */

unsigned long display_eax_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* ORIGEAX */

unsigned long change_origeax_register(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_res[1] = value;
		dbg_pr("ORIGEAX changed to 0x%lX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

/* EAX */

unsigned long change_eax_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid, width = 0;
	register unsigned long value;

	if (!strncasecmp(cmd, "AL", 2))
		width = 2;
	else if (!strncasecmp(cmd, "AX", 2))
		width = 1;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		switch (width) {
		case 0:
			dbgframe->t_ax &= ~0xFFFFFFFF;
			dbgframe->t_ax |= value & 0xFFFFFFFF;
			dbg_pr("EAX changed to 0x%lX\n", value);
			break;

		case 1:
			dbgframe->t_ax &= ~0xFFFF;
			dbgframe->t_ax |= value & 0xFFFF;
			dbg_pr("AX changed to 0x%lX\n", value);
			break;

		case 2:
			dbgframe->t_ax &= ~0xFF;
			dbgframe->t_ax |= value & 0xFF;
			dbg_pr("AL changed to 0x%lX\n", value);
			break;

		default:
			dbgframe->t_ax = value;
			break;
		}
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_ebx_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* EBX */

unsigned long change_ebx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid, width = 0;
	register unsigned long value;

	if (!strncasecmp(cmd, "BL", 2))
		width = 2;
	else
		if (!strncasecmp(cmd, "BX", 2))
			width = 1;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		switch (width) {
		case 0:
			dbgframe->t_bx &= ~0xFFFFFFFF;
			dbgframe->t_bx |= value & 0xFFFFFFFF;
			dbg_pr("EBX changed to 0x%lX\n", value);
			break;

		case 1:
			dbgframe->t_bx &= ~0xFFFF;
			dbgframe->t_bx |= value & 0xFFFF;
			dbg_pr("BX changed to 0x%lX\n", value);
			break;

		case 2:
			dbgframe->t_bx &= ~0xFF;
			dbgframe->t_bx |= value & 0xFF;
			dbg_pr("BL changed to 0x%lX\n", value);
			break;

		default:
			dbgframe->t_bx = value;
			break;
		}
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_ecx_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* ECX */

unsigned long change_ecx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid, width = 0;
	register unsigned long value;

	if (!strncasecmp(cmd, "CL", 2))
		width = 2;
	else
		if (!strncasecmp(cmd, "CX", 2))
			width = 1;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		switch (width) {
		case 0:
			dbgframe->t_cx &= ~0xFFFFFFFF;
			dbgframe->t_cx |= value & 0xFFFFFFFF;
			dbg_pr("ECX changed to 0x%lX\n", value);
			break;

		case 1:
			dbgframe->t_cx &= ~0xFFFF;
			dbgframe->t_cx |= value & 0xFFFF;
			dbg_pr("CX changed to 0x%lX\n", value);
			break;

		case 2:
			dbgframe->t_cx &= ~0xFF;
			dbgframe->t_cx |= value & 0xFF;
			dbg_pr("CL changed to 0x%lX\n", value);
			break;

		default:
			dbgframe->t_cx = value;
			break;
		}
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_edx_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* EDX */

unsigned long change_edx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid, width = 0;
	register unsigned long value;

	if (!strncasecmp(cmd, "DL", 2))
		width = 2;
	else
		if (!strncasecmp(cmd, "DX", 2))
			width = 1;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		switch (width) {
		case 0:
			dbgframe->t_dx &= ~0xFFFFFFFF;
			dbgframe->t_dx |= value & 0xFFFFFFFF;
			dbg_pr("EDX changed to 0x%lX\n", value);
			break;

		case 1:
			dbgframe->t_dx &= ~0xFFFF;
			dbgframe->t_dx |= value & 0xFFFF;
			dbg_pr("DX changed to 0x%lX\n", value);
			break;

		case 2:
			dbgframe->t_dx &= ~0xFF;
			dbgframe->t_dx |= value & 0xFF;
			dbg_pr("DL changed to 0x%lX\n", value);
			break;

		default:
			dbgframe->t_dx = value;
			break;
		}
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_esi_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* ESI */

unsigned long change_esi_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_si = value;
		dbg_pr("ESI changed to 0x%lX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_edi_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* EDI */

unsigned long change_edi_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_di = value;
		dbg_pr("EDI changed to 0x%lX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_ebp_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* EBP */

unsigned long change_ebp_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_bp = value;
		dbg_pr("EBP changed to 0x%lX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_esp_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* ESP */

unsigned long change_esp_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_sp = value;
		dbg_pr("ESP changed to 0x%lX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_eip_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* EIP */

unsigned long change_eip_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_ip = value;
		dbg_pr("EIP changed to 0x%lX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

#if IS_ENABLED(CONFIG_X86_64)
/*  */
/* X86_64 Registers */
/*  */

unsigned long display_rax_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RAX */

unsigned long change_rax_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_ax = value;
		dbg_pr("RAX changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

/* ORIGRAX */

unsigned long change_origrax_register(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_res[1] = value;
		dbg_pr("ORIGRAX changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_rbx_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RBX */

unsigned long change_rbx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_bx = value;
		dbg_pr("RBX changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_rcx_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RCX */

unsigned long change_rcx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_cx = value;
		dbg_pr("RCX changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_rdx_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RDX */

unsigned long change_rdx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_dx = value;
		dbg_pr("RDX changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_rsi_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RSI */

unsigned long change_rsi_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_si = value;
		dbg_pr("RSI changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_rdi_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RDI */

unsigned long change_rdi_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_di = value;
		dbg_pr("RDI changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_rbp_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RBP */

unsigned long change_rbp_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_bp = value;
		dbg_pr("RBP changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_rsp_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RSP */

unsigned long change_rsp_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_sp = value;
		dbg_pr("RSP changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_rip_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* RIP */

unsigned long change_rip_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->t_ip = value;
		dbg_pr("RIP changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_r8_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* R8 */

unsigned long change_r8_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->r8 = value;
		dbg_pr("R8 changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_r9_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* R9 */

unsigned long change_r9_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->r9 = value;
		dbg_pr("R9 changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_r10_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* R10 */

unsigned long change_r10_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->r10 = value;
		dbg_pr("R10 changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_r11_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* R11 */

unsigned long change_r11_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->r11 = value;
		dbg_pr("R11 changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_r12_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* R12 */

unsigned long change_r12_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->r12 = value;
		dbg_pr("R12 changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_r13_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* R13 */

unsigned long change_r13_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->r13 = value;
		dbg_pr("R13 changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_r14_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* R14 */

unsigned long change_r14_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->r14 = value;
		dbg_pr("R14 changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

unsigned long display_r15_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* R15 */

unsigned long change_r15_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbgframe->r15 = value;
		dbg_pr("R15 changed to 0x%llX\n", value);
	} else {
		dbg_pr("invalid change register command or address\n");
	}
	return 1;
}

#endif

/*  */
/* Segment Registers */
/*  */

unsigned long display_cs_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* CS */

unsigned long change_cs_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;
	register unsigned short old_w;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_w = dbgframe->t_cs;
		dbgframe->t_cs = (unsigned short)value;
		dbg_pr("CS: =[%04X] changed to CS: =[%04X]\n",
		       old_w, value);
	} else {
		dbg_pr("invalid change segment register command\n");
	}
	return 1;
}

unsigned long display_ds_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

#if !IS_ENABLED(CONFIG_X86_64)
/* DS */

unsigned long change_ds_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;
	register unsigned short old_w;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_w = dbgframe->t_ds;
		dbgframe->t_ds = (unsigned short)value;
		dbg_pr("DS: =[%04X] changed to DS: =[%04X]\n",
		       old_w, value);
	} else {
		dbg_pr("invalid change segment register command\n");
	}
	return 1;
}

unsigned long display_es_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* ES */

unsigned long change_es_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;
	register unsigned short old_w;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_w = dbgframe->t_es;
		dbgframe->t_es = (unsigned short)value;
		dbg_pr("ES: =[%04X] changed to ES: =[%04X]\n",
		       old_w, value);
	} else {
		dbg_pr("invalid change segment register command\n");
	}
	return 1;
}
#endif

unsigned long display_fs_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* FS */

unsigned long change_fs_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;
	register unsigned short old_w;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_w = dbgframe->t_fs;
		dbgframe->t_fs = (unsigned short)value;
		dbg_pr("FS: =[%04X] changed to FS: =[%04X]\n",
		       old_w, value);
	} else {
		dbg_pr("invalid change segment register command\n");
	}
	return 1;
}

unsigned long display_gs_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* GS */

unsigned long change_gs_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;
	register unsigned short old_w;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_w = dbgframe->t_gs;
		dbgframe->t_gs = (unsigned short)value;
		dbg_pr("GS: =[%04X] changed to GS: =[%04X]\n",
		       old_w, value);
	} else {
		dbg_pr("invalid change segment register command\n");
	}
	return 1;
}

unsigned long display_ss_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* SS */

unsigned long change_ss_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value;
	register unsigned short old_w;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_w = dbgframe->t_ss;
		dbgframe->t_ss = (unsigned short)value;
		dbg_pr("SS: =[%04X] changed to SS: =[%04X]\n",
		       old_w, value);
	} else {
		dbg_pr("invalid change segment register command\n");
	}
	return 1;
}

unsigned long display_rf_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* RF */

unsigned long change_rf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & RF_FLAG;
		(value) ? (dbgframe->t_flags |= RF_FLAG) :
			(dbgframe->t_flags &= ~RF_FLAG);
		dbg_pr("EFlag RF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_tf_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* TF */

unsigned long change_tf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & TF_FLAG;
		(value) ? (dbgframe->t_flags |= TF_FLAG) :
			(dbgframe->t_flags &= ~TF_FLAG);
		dbg_pr("EFlag TF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_zf_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* ZF */

unsigned long change_zf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & ZF_FLAG;
		(value) ? (dbgframe->t_flags |= ZF_FLAG) :
			(dbgframe->t_flags &= ~ZF_FLAG);
		dbg_pr("EFlag ZF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_sf_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* SF */

unsigned long change_sf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & SF_FLAG;
		(value) ? (dbgframe->t_flags |= SF_FLAG) :
			(dbgframe->t_flags &= ~SF_FLAG);
		dbg_pr("EFlag SF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_pf_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* PF */

unsigned long change_pf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & PF_FLAG;
		(value) ? (dbgframe->t_flags |= PF_FLAG) :
			(dbgframe->t_flags &= ~PF_FLAG);
		dbg_pr("EFlag PF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_cf_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* CF */

unsigned long change_cf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & CF_FLAG;
		(value) ? (dbgframe->t_flags |= CF_FLAG) :
			(dbgframe->t_flags &= ~CF_FLAG);
		dbg_pr("EFlag CF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_of_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* OF */

unsigned long change_of_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & OF_FLAG;
		(value) ? (dbgframe->t_flags |= OF_FLAG) :
			(dbgframe->t_flags &= ~OF_FLAG);
		dbg_pr("EFlag OF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_if_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* IF */

unsigned long change_if_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & IF_FLAG;
		(value) ? (dbgframe->t_flags |= IF_FLAG) :
			(dbgframe->t_flags &= ~IF_FLAG);
		dbg_pr("EFlag IF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_id_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* ID */

unsigned long change_id_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & ID_FLAGS;
		(value) ? (dbgframe->t_flags |= ID_FLAGS) :
			(dbgframe->t_flags &= ~ID_FLAGS);
		dbg_pr("EFlag ID[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_df_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* DF */

unsigned long change_df_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & DF_FLAG;
		(value) ? (dbgframe->t_flags |= DF_FLAG) :
			(dbgframe->t_flags &= ~DF_FLAG);
		dbg_pr("EFlag DF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_nt_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* NT */

unsigned long change_nt_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & NT_FLAG;
		(value) ? (dbgframe->t_flags |= NT_FLAG) :
			(dbgframe->t_flags &= ~NT_FLAG);
		dbg_pr("EFlag NT[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_vm_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* VM */

unsigned long change_vm_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & VM_FLAG;
		(value) ? (dbgframe->t_flags |= VM_FLAG) :
			(dbgframe->t_flags &= ~VM_FLAG);
		dbg_pr("EFlag VM[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_vif_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* VIF */

unsigned long change_vif_flag(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & VIF_FLAG;
		(value) ? (dbgframe->t_flags |= VIF_FLAG) :
			(dbgframe->t_flags &= ~VIF_FLAG);
		dbg_pr("EFlag VIF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_vip_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	return 1;
}

/* VIP */

unsigned long change_vip_flag(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & VIP_FLAG;
		(value) ? (dbgframe->t_flags |= VIP_FLAG) :
			(dbgframe->t_flags &= ~VIP_FLAG);
		dbg_pr("EFlag VIP[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_af_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* AF */

unsigned long change_af_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & AF_FLAG;
		(value) ? (dbgframe->t_flags |= AF_FLAG) :
			(dbgframe->t_flags &= ~AF_FLAG);
		dbg_pr("EFlag AF[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_ac_help(unsigned char *command_line,
			      dbg_parser *parser)
{
	return 1;
}

/* AC */

unsigned long change_ac_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long value, old_d;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		old_d = dbgframe->t_flags & AC_FLAG;
		(value) ? (dbgframe->t_flags |= AC_FLAG) :
			(dbgframe->t_flags &= ~AC_FLAG);
		dbg_pr("EFlag AC[%lX] changed to (%d)\n",
		       old_d, (int)value);
	} else {
		dbg_pr("invalid change flags command\n");
	}
	return 1;
}

unsigned long display_mtrr_help(unsigned char *command_line,
				dbg_parser *parser)
{
	dbg_pr("mtrr                     - display MTRR registers\n");
	return 1;
}

/* MTRR */

unsigned long show_mtrr_registers(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	display_mtrr_registers();
	return 1;
}

unsigned long display_gdt_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	dbg_pr(".g or .g <address>       - show global descriptors\n");
	return 1;
}

/* .G */

unsigned long show_gdt(unsigned char *cmd,
		       dbg_regs *dbgframe,
		       unsigned long exception,
		       dbg_parser *parser)
{
	register unsigned long address;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid)
		display_gdt((unsigned char *)address);
	else
		display_gdt((unsigned char *)0);
	return 1;
}

unsigned long display_idt_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	dbg_pr(".i or .i <address>       - show interrupt descriptors\n");
	return 1;
}

/* .I */

unsigned long show_idt(unsigned char *cmd,
		       dbg_regs *dbgframe,
		       unsigned long exception,
		       dbg_parser *parser)
{
	register unsigned long address;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid)
		display_idt((unsigned char *)address);
	else
		display_idt((unsigned char *)0);
	return 1;
}

unsigned long evaluate_expr_help(unsigned char *command_line,
				 dbg_parser *parser)
{
	display_expr_help();
	return 1;
}

/* .E */

unsigned long evaluate_expr(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser)
{
	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	eval_command_expr(dbgframe, cmd);
	return 1;
}

unsigned long port_command_help(unsigned char *command_line,
				dbg_parser *parser)
{
	dbg_pr("i   <port>               - input byte from port\n");
	dbg_pr("ib  <port>               - input byte from port\n");
	dbg_pr("iw  <port>               - input word from port\n");
	dbg_pr("il  <port>               - input double word from port\n");
	dbg_pr("o   <port> <val>         - output byte to port\n");
	dbg_pr("ob  <port> <val>         - output byte to port\n");
	dbg_pr("ow  <port> <val>         - output word to port\n");
	dbg_pr("ol  <port> <val>         - output double word to port\n");
	return 1;
}

/* IW */

unsigned long input_word_port(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned long address;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbg_pr("inportw (%04X) = %04X\n",
		       address, inw(address));
	} else {
		dbg_pr("bad port command\n");
	}
	return 1;
}

/* ID */

unsigned long input_double_port(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	register unsigned long address;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbg_pr("inportd (%04X) = %lX\n",
		       address, inl(address));
	} else {
		dbg_pr("bad port command\n");
	}
	return 1;
}

/* IB */

unsigned long input_byte_port(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned long address;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbg_pr("inportb (%04X) = %02X\n",
		       address, inb(address));
	} else {
		dbg_pr("bad port command\n");
	}
	return 1;
}

/* I */

unsigned long input_port(unsigned char *cmd,
			 dbg_regs *dbgframe,
			 unsigned long exception,
			 dbg_parser *parser)
{
	register unsigned long address;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbg_pr("inportb (%04X) = %02X\n",
		       address, inb(address));
	} else {
		dbg_pr("bad port command\n");
	}
	return 1;
}

/* OW */

unsigned long output_word_port(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long port, value;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	port = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		value = eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			dbg_pr("outportw (%04X) = %04X\n",
			       port, value);
			outw(port, value);
			return 1;
		}
	} else {
		dbg_pr("bad port command\n");
	}
	return 1;
}

/* OD */

unsigned long output_double_port(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	register unsigned long port, value;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	port = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		value = eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			dbg_pr("outportd (%04X) = %lX\n",
			       port, value);
			outl(port, value);
			return 1;
		}
	} else {
		dbg_pr("bad port command\n");
	}
	return 1;
}

/* OB */

unsigned long output_byte_port(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long port, value;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	port = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		value = eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			dbg_pr("outportb (%04X) = %02X\n",
			       port, value);
			outb(port, value);
			return 1;
		}
	} else {
		dbg_pr("bad port command\n");
	}
	return 1;
}

/* O */

unsigned long output_port(unsigned char *cmd,
			  dbg_regs *dbgframe,
			  unsigned long exception,
			  dbg_parser *parser)
{
	register unsigned long port, value;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	port = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		value = eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			dbg_pr("outportb (%04X) = %02X\n",
			       port, value);
			outb(port, value);
			return 1;
		}
	} else {
		dbg_pr("bad port command\n");
	}
	return 1;
}

unsigned long breakpoint_command_help(unsigned char *command_line,
				      dbg_parser *parser)
{
	dbg_pr("b                        - display all breakpoints\n");
	dbg_pr("b   <address>            - set execute breakpoint\n");
	dbg_pr("bc[#] (1-4)              - clear breakpoint\n");
	dbg_pr("bca                      - clear all breakpoints\n");
#if IS_ENABLED(CONFIG_X86_64)
	dbg_pr("br[#] <address>          - set read/write breakpoint ");
	dbg_pr("#=1,2,4,8 byte (br1,br2,br4,br8)\n");
	dbg_pr("bw[#] <address>          - set write only breakpoint ");
	dbg_pr("#=1,2,4,8 byte (bw1,bw2,bw4,bw8)\n");
	dbg_pr("bi[#] <address>          - set io address breakpoint ");
	dbg_pr("#=1,2,4,8 byte (bi1,bi2,bi4,bi8)\n");
#else
	dbg_pr("br[#] <address>          - set read/write breakpoint ");
	dbg_pr("#=1,2,4 byte (br1,br2,br4)\n");
	dbg_pr("bw[#] <address>          - set write only breakpoint ");
	dbg_pr("#=1,2,4 byte (bw1,bw2,bw4)\n");
	dbg_pr("bi[#] <address>          - set io address breakpoint ");
	dbg_pr("#=1,2,4 byte (bi1,bi2,bi4)\n");
#endif
	dbg_pr("bm[p#]                   - mask breaks for processor\n");
	dbg_pr("bst                      - display temp breakpoints\n");
	return 1;
}

/* BCA */

unsigned long breakpoint_clear_all(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser)
{
	register unsigned long i;

	for (i = 0; i < 4; i++) {
		break_reserved[i] = 0;
		break_points[i] = 0;
		break_type[i] = 0;
		break_length[i] = 0;
		conditional_breakpoint[i] = 0;
	}
	set_debug_registers();
	dbg_pr("all breakpoints cleared\n");

	return 1;
}

/* BC */

unsigned long breakpoint_clear(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	unsigned long valid;
	register unsigned long i, address;
	register unsigned char *symbol_name;
	register unsigned char *module_name;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint not found\n");
		return 1;
	}

	i = address;
	if (i < 4) {
		symbol_name = get_symbol_value(break_points[i], &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(break_points[i],
						      &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i 0x%p (%s %s) %s|%s cleared\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i 0x%p (%s %s) %s cleared\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		break_reserved[i] = 0;
		break_points[i] = 0;
		break_type[i] = 0;
		break_length[i] = 0;
		conditional_breakpoint[i] = 0;
		set_debug_registers();
		return 1;
	}
	dbg_pr("breakpoint out of range\n");
	return 1;
}

/* BM */

unsigned long breakpoint_mask(unsigned char *cmd,
			      dbg_regs *dbgframe, unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned long address, pnum, i;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		pnum = address;
		if (pnum < MAX_PROCESSORS && cpu_online(pnum)) {
			if (per_cpu(break_mask, pnum))
				per_cpu(break_mask, pnum) = 0;
			else
				per_cpu(break_mask, pnum) = 1;
			dbg_pr("processor %i : %s\n", (int)pnum,
			       per_cpu(break_mask, pnum) ?
			       "BREAKS_MASKED" :
			       "BREAKS_UNMASKED");
		} else {
			dbg_pr("processor (%i) invalid\n", (int)pnum);
		}
	} else {
		for (i = 0; i < MAX_PROCESSORS; i++) {
			if (cpu_online(i)) {
				dbg_pr("processor %i : %s\n", (int)i,
				       per_cpu(break_mask, i) ?
				       "BREAKS_MASKED" :
				       "BREAKS_UNMASKED");
			}
		}
	}
	return 1;
}

/* BW1 */

unsigned long breakpoint_word1(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_WRITE;
		break_length[i] = ONE_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);
		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BW2 */

unsigned long breakpoint_word2(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_WRITE;
		break_length[i] = TWO_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BW4 */

unsigned long breakpoint_word4(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_WRITE;
		break_length[i] = FOUR_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BW8 */

#if IS_ENABLED(CONFIG_X86_64)
unsigned long breakpoint_word8(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_WRITE;
		break_length[i] = EIGHT_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}
#endif

/* BW */

unsigned long breakpoint_word(unsigned char *cmd,
			      dbg_regs *dbgframe, unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_WRITE;
		break_length[i] = ONE_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BR1 */

unsigned long breakpoint_read1(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_READWRITE;
		break_length[i] = ONE_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BR2 */

unsigned long breakpoint_read2(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_READWRITE;
		break_length[i] = TWO_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BR4 */

unsigned long breakpoint_read4(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_READWRITE;
		break_length[i] = FOUR_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BR8 */

#if IS_ENABLED(CONFIG_X86_64)
unsigned long breakpoint_read8(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_READWRITE;
		break_length[i] = EIGHT_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}
#endif

/* BR */

unsigned long breakpoint_read(unsigned char *cmd,
			      dbg_regs *dbgframe, unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_READWRITE;
		break_length[i] = ONE_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BI1 */

unsigned long breakpoint_io1(unsigned char *cmd,
			     dbg_regs *dbgframe, unsigned long exception,
			     dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_IOPORT;
		break_length[i] = ONE_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BI2 */

unsigned long breakpoint_io2(unsigned char *cmd,
			     dbg_regs *dbgframe, unsigned long exception,
			     dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_IOPORT;
		break_length[i] = TWO_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BI4 */

unsigned long breakpoint_io4(unsigned char *cmd,
			     dbg_regs *dbgframe, unsigned long exception,
			     dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_IOPORT;
		break_length[i] = FOUR_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BI */

unsigned long breakpoint_io(unsigned char *cmd,
			    dbg_regs *dbgframe, unsigned long exception,
			    dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; (r < 255) && (*pB); r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_IOPORT;
		break_length[i] = ONE_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* B */

void display_breakpoint(int i)
{
	register unsigned char *symbol_name;
	register unsigned char *module_name;

	symbol_name = get_symbol_value(break_points[i], &symbuf[0],
				       MAX_SYMBOL_LEN);
	module_name = get_module_symbol_value(break_points[i],
					      &modbuf[0], MAX_SYMBOL_LEN);
	if (module_name)
		dbg_pr("Break %i is at 0x%p (%s %s) %s|%s\n",
		       (int)i,
		       (unsigned int *)break_points[i],
		       break_description[(break_type[i] & 3)],
		       break_length_description[(break_length[i] & 3)],
		       ((char *)(module_name) ? (char *)(module_name)
			: (char *)("")),
		       ((char *)(symbol_name) ? (char *)(symbol_name)
			: (char *)("")));
	else
		dbg_pr("Break %i is at 0x%p (%s %s) %s\n",
		       (int)i,
		       (unsigned int *)break_points[i],
		       break_description[(break_type[i] & 3)],
		       break_length_description[(break_length[i] & 3)],
		       ((char *)(symbol_name) ? (char *)(symbol_name)
			: (char *)("")));

	if (conditional_breakpoint[i])
		dbg_pr("if (%s) is TRUE\n", break_condition[i]);
}

unsigned long breakpoint_execute(unsigned char *cmd,
				 dbg_regs *dbgframe, unsigned long exception,
				 dbg_parser *parser)
{
	register unsigned long address, i, r;
	register unsigned char *pB, *symbol_name;
	register unsigned char *module_name;
	unsigned long valid, found = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (!*cmd) {
		for (i = 0; i < 4; i++) {
			if (break_reserved[i]) {
				display_breakpoint(i);
				found = 1;
			}
		}
		if (!found)
			dbg_pr("no breakpoints currently defined\n");

		return 1;
	}

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("breakpoint parameters invalid\n");
		return 1;
	}

	for (i = 0; i < 4; i++) {
		if (!break_reserved[i]) {
			found = 1;
			break;
		}
	}

	if (found) {
		pB = cmd;
		eval_expr(dbgframe, &cmd, &valid);
		if (valid) {
			conditional_breakpoint[i] = 1;
			for (r = 0; r < 255 && *pB; r++)
				break_condition[i][r] = *pB++;
			break_condition[i][r] = '\0';
		}
		break_reserved[i] = 1;
		break_points[i] = address;
		break_type[i] = BREAK_EXECUTE;
		break_length[i] = ONE_BYTE_FIELD;
		symbol_name = get_symbol_value(address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(address, &modbuf[0],
						      MAX_SYMBOL_LEN);
		if (module_name)
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s|%s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(module_name) ? (char *)(module_name)
				: (char *)("")),
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));
		else
			dbg_pr("breakpoint %i set to 0x%p (%s %s) %s\n",
			       (int)i,
			       (unsigned int *)break_points[i],
			       break_description[(break_type[i] & 3)],
			       break_length_description[(break_length[i] & 3)],
			       ((char *)(symbol_name) ? (char *)(symbol_name)
				: (char *)("")));

		if (conditional_breakpoint[i])
			dbg_pr("if (%s) is TRUE\n", break_condition[i]);

		set_debug_registers();
		return 1;
	}
	dbg_pr("no breakpoint available\n");
	return 1;
}

/* BST */

void display_temp_break(int i)
{
	register unsigned char *symbol_name;
	register unsigned char *module_name;

	symbol_name = get_symbol_value(break_points[i], &symbuf[0],
				       MAX_SYMBOL_LEN);
	module_name = get_module_symbol_value(break_points[i],
					      &modbuf[0], MAX_SYMBOL_LEN);
	if (module_name)
		dbg_pr("Break %i is at 0x%p (%s %s) %s|%s[%s]\n",
		       (int)i,
		       (unsigned int *)break_points[i],
		       break_description[(break_type[i] & 3)],
		       break_length_description[(break_length[i] & 3)],
		       ((char *)(module_name) ? (char *)(module_name)
			: (char *)("")),
		       ((char *)(symbol_name) ? (char *)(symbol_name)
			: (char *)("")),
		       break_go[i] ? "GO" : break_proceed[i]
		       ? "PROCEED" : "");
	else
		dbg_pr("Break %i is at 0x%p (%s %s) %s[%s]\n",
		       (int)i,
		       (unsigned int *)break_points[i],
		       break_description[(break_type[i] & 3)],
		       break_length_description[(break_length[i] & 3)],
		       ((char *)(symbol_name) ? (char *)(symbol_name)
			: (char *)("")),
		       break_go[i] ? "GO" : break_proceed[i]
		       ? "PROCEED" : "");
	if (conditional_breakpoint[i])
		dbg_pr("if (%s) is TRUE\n", break_condition[i]);
}

unsigned long breakpoint_show_temp(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser)
{
	register unsigned long i;
	register int found = 0;

	for (i = 0; i < 4; i++) {
		if (break_reserved[i] && break_temp[i]) {
			display_temp_break(i);
			found = 1;
		}
	}
	if (!found)
		dbg_pr("no temporary breakpoints defined\n");

	return 1;
}

unsigned long display_processor_status_help(unsigned char *command_line,
					    dbg_parser *parser)
{
	dbg_pr("displays active processors and their current state\n");
	return 1;
}

unsigned long display_processor_status(unsigned char *cmd,
				       dbg_regs *dbgframe,
				       unsigned long exception,
				       dbg_parser *parser)
{
	register unsigned long i;

	for (i = 0; i < MAX_PROCESSORS; i++) {
		if (cpu_online(i)) {
			dbg_pr("Processor: (%i)  State:  %s\n",
			       i,
			       proc_state[per_cpu(processor_state, i) & 0x0F]);
		}
	}
	return 1;
}

void display_mtrr_registers(void)
{
	register int i;
	unsigned long base1, base2;
	unsigned long mask1, mask2;

	if (cpu_mttr_on()) {
		dbg_pr("memory type range registers\n");
		for (i = 0; i < 8; i++) {
			read_msr(MTRR_BASE_REGS[i], &base1, &base2);
			read_msr(MTRR_MASK_VALUES[i], &mask1, &mask2);
			dbg_pr("MTRR_BASE_%i  %lX:%lX   ",
			       (int)i,
			       base1, base2);

			dbg_pr("MTRR_MASK_%i  %lX:%lX\n",
			       (int)i,
			       mask1, mask2);
		}
	} else {
		dbg_pr("memory type range registers not supported\n");
	}
}

void display_gdt(unsigned char *GDT_ADDRESS)
{
	register int i, r;
	unsigned long count;
	unsigned long gdt_pointer;
	unsigned short gdt_index;
	unsigned char *p;
	unsigned char GDTR[16];
	GDT *gdt;
#if !IS_ENABLED(CONFIG_X86_64)
	TSS *tss;
#endif
	union {
		GDT lgdt;
		unsigned char data[16];
	} lg;

#if IS_ENABLED(CONFIG_X86_64)
	read_gdtr((unsigned long *)&GDTR[0]);
	gdt_index = mdb_getword((unsigned long)&GDTR[0], 2);
	gdt_pointer = mdb_getword((unsigned long)&GDTR[2], 8);

	dbg_pr("GDTR: %04X:%llX  Processor: %i\n",
	       gdt_index, (unsigned long long)gdt_pointer,
	       (int)get_processor_id());

	count = 0;
	gdt_index = (gdt_index + 7) / 8;
	p = (unsigned char *)gdt_pointer;
	for (i = 0; i < gdt_index; i++) {
		if (dbg_pr("%04X(%04i):", count, (int)i))
			return;
		for (r = 0; r < 16; r++)
			lg.data[r] =
				(unsigned char)mdb_getword((unsigned long)
							   &p[r], 1);

		gdt = (GDT *)&lg.lgdt;
		if (dbg_pr("0x%08lX%04X%04X seg:%04X dpl:%02X type:%02X ",
			   gdt->offset_high,
			   gdt->offset_middle,
			   gdt->offset_low,
			   gdt->segment,
			   gdt->dpl,
			   gdt->type))
			return;

		if (dbg_pr("p:%02X ist:%02X",
			   gdt->p,
			   gdt->ist))
			return;

		if (dbg_pr("\n"))
			return;

		p = (void *)((unsigned long)p + 16);
		count += 16;
	}

#else
	read_gdtr((unsigned long *)&GDTR[0]);
	gdt_index = mdb_getword((unsigned long)&GDTR[0], 2);
	gdt_pointer = mdb_getword((unsigned long)&GDTR[2], 4);

	dbg_pr("GDTR: %04X:%lX  Processor: %i\n",
	       gdt_index, gdt_pointer,
	       (int)get_processor_id());

	count = 0;
	gdt_index = (gdt_index + 7) / 8;
	p = (unsigned char *)gdt_pointer;
	for (i = 0; i < gdt_index; i++) {
		if (dbg_pr("%lX (%04i):", count, (int)i))
			return;
		for (r = 0; r < 8; r++) {
			lg.data[r] =
				(unsigned char)mdb_getword((unsigned long)
							   &p[r], 1);
			if (dbg_pr(" %02X", (unsigned char)lg.data[r]))
				return;
		}

		gdt = (GDT *)&lg.lgdt;
		if ((gdt->gdt_type & 0x92) == 0x92) {
			if (dbg_pr("  b:%lX lim:%lX t:%02X ot:%02X",
				   ((gdt->base_3 << 24) | (gdt->base_2 << 16) |
				    (gdt->base_1)),
				   (((gdt->other_type & 0xF) << 16) |
				    (gdt->limit)),
				   gdt->gdt_type, gdt->other_type))
				return;
		} else if ((gdt->gdt_type & 0x89) == 0x89) {
			tss = (TSS *)gdt;
			if (dbg_pr("  tss:%lX lim:%04X t:%02X ot:%02X",
				   ((tss->tss_base_3 << 24) |
				    (tss->tss_base_2 << 16) |
				    (tss->tss_base_1)),
				   tss->tss_limit, tss->tss_type,
				   tss->tss_other_type))
				return;
		}
		if (dbg_pr("\n"))
			return;

		p = (void *)((unsigned long)p + 8);
		count += 8;
	}
#endif
}

void display_idt(unsigned char *IDT_ADDRESS)
{
	register int i, r;
	unsigned long count;
	unsigned long idt_pointer;
	unsigned short idt_index;
	unsigned char *p;
	unsigned char IDTR[16];
	IDT *idt;
#if !IS_ENABLED(CONFIG_X86_64)
	TSS_GATE *tss_gate;
#endif
	union {
		IDT lidt;
		unsigned char data[16];
	} id;

#if IS_ENABLED(CONFIG_X86_64)
	read_idtr((unsigned long *)&IDTR[0]);
	idt_index = mdb_getword((unsigned long)&IDTR[0], 2);
	idt_pointer = mdb_getword((unsigned long)&IDTR[2], 8);

	dbg_pr("IDTR: %04X:%llX  Processor: %i\n",
	       idt_index, (unsigned long long)idt_pointer,
	       (int)get_processor_id());

	count = 0;
	idt_index = (idt_index + 7) / 8;
	p = (unsigned char *)idt_pointer;
	for (i = 0; i < idt_index; i++) {
		unsigned char *symbol_name, *module_name;
		unsigned long idt_address, temp;

		if (dbg_pr("%04X(%04i):", count, (int)i))
			return;
		for (r = 0; r < 16; r++)
			id.data[r] = mdb_getword((unsigned long)&p[r], 1);

		idt = (IDT *)&id.lidt;
		if (dbg_pr("0x%08lX%04X%04X seg:%04X dpl:%02X ",
			   idt->offset_high,
			   idt->offset_middle,
			   idt->offset_low,
			   idt->segment,
			   idt->dpl))
			return;

		if (dbg_pr("type:%02X p:%02X ist:%02X",
			   idt->type,
			   idt->p,
			   idt->ist))
			return;

		idt_address = idt->offset_high;
		idt_address <<= 32;
		temp = idt->offset_middle;
		temp <<= 16;
		idt_address |= temp;
		idt_address |= idt->offset_low;

		symbol_name = get_symbol_value(idt_address, &symbuf[0],
					       MAX_SYMBOL_LEN);
		module_name = get_module_symbol_value(idt_address,
						      &modbuf[0],
						      MAX_SYMBOL_LEN);

		if (module_name)
			dbg_pr(" %s|%s", ((char *)(module_name) ?
					  (char *)(module_name) :
					  (char *)("")),
			       ((char *)(symbol_name) ?
				(char *)(symbol_name) :
				(char *)("")));
		else
			dbg_pr(" %s", ((char *)(symbol_name) ?
				       (char *)(symbol_name) :
				       (char *)("")));

		if (dbg_pr("\n"))
			return;

		p = (void *)((unsigned long)p + 16);
		count += 16;
	}
#else
	read_idtr((unsigned long *)&IDTR[0]);
	idt_index = mdb_getword((unsigned long)&IDTR[0], 2);
	idt_pointer = mdb_getword((unsigned long)&IDTR[2], 4);

	dbg_pr("IDTR: %04X:%lX  Processor: %i\n",
	       idt_index, idt_pointer,
	       (int)get_processor_id());

	count = 0;
	idt_index = (idt_index + 7) / 8;
	p = (unsigned char *)idt_pointer;
	for (i = 0; i < idt_index; i++) {
		unsigned char *symbol_name, *module_name;
		unsigned long idt_address;

		if (dbg_pr("%lX (%04i):", count, (int)i))
			return;
		for (r = 0; r < 8; r++) {
			id.data[r] = mdb_getword((unsigned long)&p[r], 1);
			if (dbg_pr(" %02X", (unsigned char)id.data[r]))
				return;
		}
		idt = (IDT *)&id.lidt;
		if ((idt->idt_flags & 0x8E) == 0x8E) {
			if (dbg_pr("  b:%lX s:%04X t:%02X ot:%02X",
				   ((idt->idt_high << 16) | (idt->idt_low)),
				   idt->idt_segment,
				   idt->idt_flags, idt->idt_skip))
				return;

			idt_address = idt->idt_high << 16 | idt->idt_low;

			symbol_name = get_symbol_value(idt_address, &symbuf[0],
						       MAX_SYMBOL_LEN);
			module_name =
				get_module_symbol_value(idt_address,
							&modbuf[0],
							MAX_SYMBOL_LEN);

			if (module_name)
				dbg_pr(" %s|%s", ((char *)(module_name) ?
						  (char *)(module_name) :
						  (char *)("")),
				       ((char *)(symbol_name) ?
					(char *)(symbol_name) :
					(char *)("")));
			else
				dbg_pr(" %s", ((char *)(symbol_name) ?
					       (char *)(symbol_name) :
					       (char *)("")));
		} else if ((idt->idt_flags & 0x85) == 0x85) {
			tss_gate = (TSS_GATE *)idt;
			if (dbg_pr("  task_gate: %04X t:%02X",
				   tss_gate->tss_selector, tss_gate->tss_flags))
				return;
		}

		if (dbg_pr("\n"))
			return;

		p = (void *)((unsigned long)p + 8);
		count += 8;
	}
#endif
}

void display_tss(dbg_regs *dbgframe)
{
#if IS_ENABLED(CONFIG_X86_64)
	unsigned long i, f = 0;

	dbg_pr("Task State Segment at 0x%p\n", dbgframe);

	dbg_pr("RAX: %016lX  RBX: %016lX  RCX: %016lX\n",
	       dbgframe->t_ax, dbgframe->t_bx, dbgframe->t_cx);

	dbg_pr("RDX: %016lX  RSI: %016lX  RDI: %016lX\n",
	       dbgframe->t_dx, dbgframe->t_si, dbgframe->t_di);

	dbg_pr("RSP: %016lX  RBP: %016lX   R8: %016lX\n",
	       dbgframe->t_sp, dbgframe->t_bp, dbgframe->r8);

	dbg_pr(" R9: %016lX  R10: %016lX  R11: %016lX\n",
	       dbgframe->r9, dbgframe->r10, dbgframe->r11);

	dbg_pr("R12: %016lX  R13: %016lX  R14: %016lX\n",
	       dbgframe->r12, dbgframe->r13, dbgframe->r14);

	dbg_pr("R15: %016lX\n", dbgframe->r15);

	dbg_pr("CS: %04X  DS: %04X  ES: %04X  FS: %04X  GS: %04X ",
	       dbgframe->t_cs, dbgframe->t_ds,
	       dbgframe->t_es, dbgframe->t_fs,
	       dbgframe->t_gs);

	dbg_pr("SS: %04X  LDT: %04X\n",
	       dbgframe->t_ss,
	       dbgframe->t_ldt);

	dbg_pr("RIP: %016lX  FLAGS: %016lX ",
	       dbgframe->t_ip, dbgframe->t_flags);

	dbg_pr(" (");
	for (i = 0; i < 22; i++) {
		if (ia_flags[i]) {
			if ((dbgframe->t_flags >> i) & 0x00000001) {
				if (f)
					dbg_pr(" ");
				f = 1;
				dbg_pr("%s", ia_flags[i]);
			}
		}
	}
	dbg_pr(")\n");

	dbg_pr("CR3: %016lX  IOMAP: %016lX  BLINK: %016lX\n",
	       dbgframe->t_cr3, dbgframe->t_io_map,
	       dbgframe->t_res[0]);

#else
	unsigned long i, f = 0;

	dbg_pr("Task State Segment at 0x%p\n", dbgframe);

	dbg_pr("EAX: %08X  EBX: %08X  ECX: %08X  EDX: %08X\n",
	       dbgframe->t_ax, dbgframe->t_bx,
	       dbgframe->t_cx, dbgframe->t_dx);

	dbg_pr("ESI: %08X  EDI: %08X  ESP: %08X  EBP: %08X\n",
	       dbgframe->t_si, dbgframe->t_di,
	       dbgframe->t_sp, dbgframe->t_bp);

	dbg_pr("CS: %04X  DS: %04X  ES: %04X  FS: %04X  GS: %04X  ",
	       dbgframe->t_cs, dbgframe->t_ds,
	       dbgframe->t_es, dbgframe->t_fs,
	       dbgframe->t_gs);

	dbg_pr("SS: %04X  LDT: %04X\n",
	       dbgframe->t_ss,
	       dbgframe->t_ldt);

	dbg_pr("EIP: %08X  FLAGS: %08X ",
	       dbgframe->t_ip,
	       dbgframe->t_flags);

	dbg_pr(" (");
	for (i = 0; i < 22; i++) {
		if (ia_flags[i]) {
			if ((dbgframe->t_flags >> i) & 0x00000001) {
				if (f)
					dbg_pr(" ");
				f = 1;
				dbg_pr("%s", ia_flags[i]);
			}
		}
	}
	dbg_pr(")\n");

	dbg_pr("CR3: %08X  IOMAP: %08X  BLINK: %08X\n",
	       dbgframe->t_cr3, dbgframe->t_io_map,
	       dbgframe->t_res[0]);

#endif
}

void display_general_registers(dbg_regs *dbgframe)
{
#if IS_ENABLED(CONFIG_X86_64)
	unsigned long i, f = 0;

	dbg_pr("RAX: %016lX ", dbgframe->t_ax);
	dbg_pr("RBX: %016lX ", dbgframe->t_bx);
	dbg_pr("RCX: %016lX\n", dbgframe->t_cx);
	dbg_pr("RDX: %016lX ", dbgframe->t_dx);
	dbg_pr("RSI: %016lX ", dbgframe->t_si);
	dbg_pr("RDI: %016lX\n", dbgframe->t_di);
	dbg_pr("RSP: %016lX ", dbgframe->t_sp);
	dbg_pr("RBP: %016lX ", dbgframe->t_bp);
	dbg_pr(" R8: %016lX\n", dbgframe->r8);
	dbg_pr(" R9: %016lX ", dbgframe->r9);
	dbg_pr("R10: %016lX ", dbgframe->r10);
	dbg_pr("R11: %016lX\n", dbgframe->r11);
	dbg_pr("R12: %016lX ", dbgframe->r12);
	dbg_pr("R13: %016lX ", dbgframe->r13);
	dbg_pr("R14: %016lX\n", dbgframe->r14);
	dbg_pr("R15: %016lX ", dbgframe->r15);

	if (segment_toggle)
		display_segment_registers(dbgframe);
	else
		dbg_pr("\n");

	dbg_pr(" IP: %016lX ", dbgframe->t_ip);
	dbg_pr("FLAGS: %016lX ", dbgframe->t_flags);

	dbg_pr(" (");
	for (i = 0; i < 22; i++) {
		if (ia_flags[i]) {
			if ((dbgframe->t_flags >> i) & 0x00000001) {
				if (f)
					dbg_pr(" ");
				f = 1;
				dbg_pr("%s", ia_flags[i]);
			}
		}
	}
	dbg_pr(")\n");

#else
	unsigned long i, f = 0;

	dbg_pr("EAX: %08X ", dbgframe->t_ax);
	dbg_pr("EBX: %08X ", dbgframe->t_bx);
	dbg_pr("ECX: %08X ", dbgframe->t_cx);
	dbg_pr("EDX: %08X\n", dbgframe->t_dx);
	dbg_pr("ESI: %08X ", dbgframe->t_si);
	dbg_pr("EDI: %08X ", dbgframe->t_di);
	dbg_pr("ESP: %08X ", dbgframe->t_sp);
	dbg_pr("EBP: %08X\n", dbgframe->t_bp);

	if (segment_toggle)
		display_segment_registers(dbgframe);

	dbg_pr("EIP: %08X ", dbgframe->t_ip);
	dbg_pr("EFLAGS: %08X ", dbgframe->t_flags);

	dbg_pr(" (");
	for (i = 0; i < 22; i++) {
		if (ia_flags[i]) {
			if ((dbgframe->t_flags >> i) & 0x00000001) {
				if (f)
					dbg_pr(" ");
				f = 1;
				dbg_pr("%s", ia_flags[i]);
			}
		}
	}
	dbg_pr(")\n");
#endif
}

void display_segment_registers(dbg_regs *dbgframe)
{
#if IS_ENABLED(CONFIG_X86_64)
	dbg_pr("CS: %04X ", dbgframe->t_cs);
	dbg_pr("DS: %04X ", dbgframe->t_ds);
	dbg_pr("ES: %04X ", dbgframe->t_es);
	dbg_pr("FS: %04X ", dbgframe->t_fs);
	dbg_pr("GS: %04X ", dbgframe->t_gs);
	dbg_pr("SS: %04X\n", dbgframe->t_ss);
#else
	dbg_pr("CS: %04X ", dbgframe->t_cs);
	dbg_pr("DS: %04X ", dbgframe->t_ds);
	dbg_pr("ES: %04X ", dbgframe->t_es);
	dbg_pr("FS: %04X ", dbgframe->t_fs);
	dbg_pr("GS: %04X ", dbgframe->t_gs);
	dbg_pr("SS: %04X\n", dbgframe->t_ss);
#endif
}

void display_control_registers(unsigned long processor,
			       dbg_regs *dbgframe)
{
	unsigned char GDTR[16], IDTR[16];

#if IS_ENABLED(CONFIG_X86_64)
	dbg_pr("CR0: %016lX ", dbg_read_cr0());
	dbg_pr("CR2: %016lX ", dbg_read_cr2());
	dbg_pr("CR3: %016lX\n", dbg_read_cr3());
	dbg_pr("CR4: %016lX ", dbg_read_cr4());
	dbg_pr("DR0: %016lX ", dbg_read_dr0());
	dbg_pr("DR1: %016lX\n", dbg_read_dr1());
	dbg_pr("DR2: %016lX ", dbg_read_dr2());
	dbg_pr("DR3: %016lX ", dbg_read_dr3());
	dbg_pr("DR6: %016lX\n", dbg_read_dr6());
	dbg_pr("DR7: %016lX ", dbg_read_dr7());
	dbg_pr("VR6: %016lX ", per_cpu(current_dr6,
				       processor));
	dbg_pr("VR7: %016lX\n", current_dr7);

	read_gdtr((unsigned long *)&GDTR[0]);
	read_idtr((unsigned long *)&IDTR[0]);

	dbg_pr("GDTR: %04X:%llX IDTR: %04X:%llX\n",
	       *(unsigned short *)&GDTR[0],
	       *(unsigned long long *)&GDTR[2],
	       *(unsigned short *)&IDTR[0],
	       *(unsigned long long *)&IDTR[2]);
	dbg_pr("LDTR: %04X  TR: %04X\n",
	       read_ldtr(),
	       read_tr());
#else
	dbg_pr("CR0: %08X ", dbg_read_cr0());
	dbg_pr("CR2: %08X ", dbg_read_cr2());
	dbg_pr("CR3: %08X ", dbg_read_cr3());
	dbg_pr("CR4: %08X\n", dbg_read_cr4());
	dbg_pr("DR0: %08X ", dbg_read_dr0());
	dbg_pr("DR1: %08X ", dbg_read_dr1());
	dbg_pr("DR2: %08X ", dbg_read_dr2());
	dbg_pr("DR3: %08X\n", dbg_read_dr3());
	dbg_pr("DR6: %08X ", dbg_read_dr6());
	dbg_pr("DR7: %08X ", dbg_read_dr7());
	dbg_pr("VR6: %08X ", per_cpu(current_dr6,
				     processor));
	dbg_pr("VR7: %08X\n", current_dr7);

	dbg_pr("GDTR: %04X:%08X IDTR: %04X:%08X  LDTR: %04X  TR: %04X\n",
	       *(unsigned short *)&GDTR[0],
	       *(unsigned long *)&GDTR[2],
	       *(unsigned short *)&IDTR[0],
	       *(unsigned long *)&IDTR[2],
	       read_ldtr(),
	       read_tr());
#endif
}

unsigned long find_bp_index(unsigned long exception,
			    dbg_regs *dbgframe,
			    unsigned long processor)
{
	register int i;

	switch (exception) {
	case DEBUGGER_EXCEPTION:
		for (i = 0; i < 4; i++) {
			if (per_cpu(current_dr6, processor) & (1 << i))
				return i;
		}
		break;

	default:
		break;
	}
	return -1;
}

unsigned long console_display_reason(dbg_regs *dbgframe,
				     unsigned long exception,
				     unsigned long processor,
				     unsigned long last_cmd)
{
	register int i = find_bp_index(exception, dbgframe, processor);

	if (last_mdb_oops)
		dbg_pr("\nKernel Oops reported (%s)\n", last_mdb_oops);

	switch (exception) {
	case DEBUGGER_EXCEPTION:
		if (i != -1 && break_go[i])
			dbg_pr("\nBreak at 0x%lX - GO breakpoint (%d)\n",
			       dbgframe->t_ip, i);
		else if (i != -1 && break_proceed[i])
			dbg_pr("\nBreak at 0x%lX - Proceed (break) (%d)\n",
			       dbgframe->t_ip, i);
		else if (i != -1 && break_points[i] &&
			 conditional_breakpoint[i]) {
			dbg_pr("\nBreak at 0x%lX - breakpoint %d (%s)\n",
			       dbgframe->t_ip, i,
			       break_description[(break_type[i] & 3)]);
			dbg_pr("expr: %s was TRUE\n", break_condition[i]);
		} else if (i != -1 && break_points[i])
			dbg_pr("\nBreak at 0x%lX - breakpoint %d (%s)\n",
			       dbgframe->t_ip, i,
			       break_description[(break_type[i] & 3)]);
		else if (last_cmd_key == 'P' || last_cmd_key == K_F8)
			dbg_pr("\nBreak at 0x%lX - Proceed (single step)\n",
			       dbgframe->t_ip);
		else if (last_cmd_key == 'T' || last_cmd_key == K_F7)
			dbg_pr("\nBreak at 0x%lX - Trace (single step)\n",
			       dbgframe->t_ip);
		else if (last_cmd_key == K_F6)
			dbg_pr("\nBreak at 0x%lX - SSB (step til branch)\n",
			       dbgframe->t_ip);
		else
			dbg_pr("\nBreak at 0x%lX - INT1 breakpoint\n",
			       dbgframe->t_ip);
		break;

	case BREAKPOINT_EXCEPTION:
		dbg_pr("\nBreak at 0x%lX - INT3 breakpoint\n",
		       dbgframe->t_ip);
		break;

	default:
		if ((exception < exceptions) &&
		    exception_description[exception % exceptions])
			dbg_pr("\nBreak at 0x%lX - %s\n",
			       dbgframe->t_ip,
			       exception_description[exception % exceptions]);
		else
			dbg_pr("\nBreak at 0x%lX due to - Unknown Reason\n",
			       dbgframe->t_ip);
		break;
	}
	return 1;
}

unsigned long reason_help(unsigned char *command_line, dbg_parser *parser)
{
	dbg_pr("display break reason\n");
	return 1;
}

unsigned long reason_display(unsigned char *cmd,
			     dbg_regs *dbgframe, unsigned long exception,
			     dbg_parser *parser)
{
	console_display_reason(dbgframe, exception, get_processor_id(), 0);
	return 1;
}

void read_dbg_regs(void *frame, dbg_regs *sf, unsigned long processor)
{
	struct pt_regs *regs = frame;

	sf->t_res[1] = regs->orig_ax;
	sf->t_res[2] = (unsigned long)regs;
	sf->t_cr3 = dbg_read_cr3();
	sf->t_ldt = read_ldtr();

	sf->t_ip = regs->ip;
	sf->t_ax = regs->ax;
	sf->t_bx = regs->bx;
	sf->t_cx = regs->cx;
	sf->t_dx = regs->dx;
	sf->t_si = regs->si;
	sf->t_di = regs->di;
	sf->t_bp = regs->bp;

#if IS_ENABLED(CONFIG_X86_32)
	sf->t_flags = regs->flags;
	sf->t_cs = regs->cs;
	sf->t_ds = regs->ds;
	sf->t_es = regs->es;
	sf->t_fs = read_fs();
	sf->t_gs = read_gs();
	if (user_mode(regs)) {
		sf->t_ss = regs->ss;
		sf->t_sp = regs->sp;
	} else {
		sf->t_ss = __KERNEL_DS;
		sf->t_sp = kernel_stack_pointer(regs);
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

	sf->t_flags = regs->flags;
	sf->t_cs = regs->cs;
	sf->t_ds = read_ds();
	sf->t_es = read_es();
	sf->t_fs = read_fs();
	sf->t_gs = read_gs();
	sf->t_ss = regs->ss;
	sf->t_sp = regs->sp;
#endif
}

void read_task_frame(dbg_regs *sf, struct task_struct *p)
{
	memset((void *)sf, 0, sizeof(dbg_regs));

	sf->t_bp = *(unsigned long *)p->thread.sp;
#if IS_ENABLED(CONFIG_X86_32)
	sf->t_ds = __KERNEL_DS;
	sf->t_es = __KERNEL_DS;
	sf->t_cs = __KERNEL_CS;
	sf->t_ss = __KERNEL_DS;
	sf->t_fs = 0xFFFF;
	sf->t_gs = 0xFFFF;
	sf->t_ip = p->thread.ip;
	sf->t_flags = 0;
#else
	sf->t_cs = __KERNEL_CS;
	sf->t_ss = __KERNEL_DS;
	sf->t_ip = 0;
	sf->t_flags = *(unsigned long *)(p->thread.sp + 8);
#endif
	sf->t_sp = p->thread.sp;
}

void write_dbg_regs(void *frame, dbg_regs *sf,
		    unsigned long processor)
{
	struct pt_regs *regs = frame;

	regs->ip = sf->t_ip;
	regs->ax = sf->t_ax;
	regs->bx = sf->t_bx;
	regs->cx = sf->t_cx;
	regs->dx = sf->t_dx;
	regs->si = sf->t_si;
	regs->di = sf->t_di;
	regs->bp = sf->t_bp;

#if IS_ENABLED(CONFIG_X86_32)
	regs->flags = sf->t_flags;
	regs->cs = sf->t_cs;
	regs->ds = sf->t_ds;
	regs->es = sf->t_es;
#else
	regs->r8 = sf->r8;
	regs->r9 = sf->r9;
	regs->r10 = sf->r10;
	regs->r11 = sf->r11;
	regs->r12 = sf->r12;
	regs->r13 = sf->r13;
	regs->r14 = sf->r14;
	regs->r15 = sf->r15;

	regs->flags = sf->t_flags;
	regs->cs = sf->t_cs;
	regs->ss = sf->t_ss;
#endif
}

/*   processor synchronization
 *
 *   We have to handle multiple cpus with active breakpoints
 *   attempting to access the debugger.  We also have to handle
 *   double faulted situations.
 *
 */

unsigned long is_processor_held(unsigned long cpu)
{
	return per_cpu(processor_hold, cpu) ||
		(per_cpu(processor_state, cpu)) == PROCESSOR_SUSPEND;
}

unsigned long debug_lock(spinlock_t *lock, rlock_t *rlock, unsigned long p)
{
#if IS_ENABLED(CONFIG_SMP)
	if (!spin_trylock_irqsave((spinlock_t *)lock, rlock->flags[p])) {
		if (rlock->processor == p) {
			rlock->count++;
		} else {
			while (1) {
				per_cpu(processor_state, p) = PROCESSOR_WAIT;
				while (atomic_read(&focus_active) &&
				       !atomic_read(&per_cpu(trace_processors,
							     p))) {
					cpu_relax();
					mdb_watchdogs();
				}

				if (spin_trylock_irqsave((spinlock_t *)lock,
							 rlock->flags[p]))
					break;

				mdb_watchdogs();
			}
			per_cpu(processor_state, p) = PROCESSOR_DEBUG;
			rlock->processor = p;
		}
	} else {
		rlock->processor = p;
	}

#endif /* CONFIG_SMP */
	return 1;
}

void debug_unlock(spinlock_t *lock, rlock_t *rlock, unsigned long p)
{
#if IS_ENABLED(CONFIG_SMP)
	if (rlock->count) {
		rlock->count--;
	} else {
		rlock->processor = -1;
		spin_unlock_irqrestore((spinlock_t *)lock, rlock->flags[p]);
	}
#endif /* CONFIG_SMP */
}

unsigned long debug_rlock(spinlock_t *lock, rlock_t *rlock, unsigned long p)
{
#if IS_ENABLED(CONFIG_SMP)
	if (!spin_trylock_irqsave((spinlock_t *)lock, rlock->flags[p])) {
		if (rlock->processor == p) {
			rlock->count++;
		} else {
			while (1) {
				if (spin_trylock_irqsave((spinlock_t *)lock,
							 rlock->flags[p]))
					break;

				cpu_relax();
				mdb_watchdogs();
			}
			rlock->processor = p;
		}
	} else {
		rlock->processor = p;
	}

#endif /* CONFIG_SMP */
	return 1;
}

void debug_unrlock(spinlock_t *lock, rlock_t *rlock, unsigned long p)
{
#if IS_ENABLED(CONFIG_SMP)
	if (rlock->count) {
		rlock->count--;
	} else {
		rlock->processor = -1;
		spin_unlock_irqrestore((spinlock_t *)lock, rlock->flags[p]);
	}
#endif /* CONFIG_SMP */
}

unsigned long stop_processors_excl_self(unsigned long self)
{
#if IS_ENABLED(CONFIG_SMP) && !IS_ENABLED(CONFIG_X86_ELAN)
	register unsigned long failed;
	register int i;

	mdelay(1);

	for (i = 0; i < MAX_PROCESSORS; i++)  {
		if (cpu_online(i) && (i != self)) {
			if (!atomic_read(&per_cpu(debugger_processors, i))) {
				/* serialize processors */
				smp_mb__before_atomic();
				per_cpu(processor_hold, i) = 1;
				/* serialize processors */
				smp_mb__after_atomic();

				apic->send_IPI_mask(cpumask_of(i), APIC_DM_NMI);
			}
		}
	}

	for (i = 0, failed = 0; i < MAX_PROCESSORS; i++) {
		if (cpu_online(i) && (i != self)) {
			register unsigned long msecs = 3000;

			while (!atomic_read(&per_cpu(debugger_processors, i)) &&
			       msecs) {
				mdelay(1);
				msecs--;
			}

			if (!msecs) {
				failed++;
				dbg_pr("Processor %i could not be halted\n",
				       (int)i);
			}
		}
	}

	return (unsigned long)failed;
#else
	return 0;
#endif
}

unsigned long free_processors_excl_self(unsigned long self)
{
#if IS_ENABLED(CONFIG_SMP) && !IS_ENABLED(CONFIG_X86_ELAN)
	register int i;

	for (i = 0; i < MAX_PROCESSORS; i++) {
		if (per_cpu(processor_state, i) != PROCESSOR_WAIT) {
			/* serialize processors */
			smp_mb__before_atomic();
			per_cpu(processor_state, i) = PROCESSOR_RESUME;
			/* serialize processors */
			smp_mb__after_atomic();
		}
	}
	return i;
#else
	return MAX_PROCESSORS;
#endif
}

unsigned long wait_restart_excl_self(unsigned long self)
{
#if IS_ENABLED(CONFIG_SMP)
	register unsigned long failed;
	register int i;

	for (i = 0, failed = 0; i < MAX_PROCESSORS; i++) {
		if (cpu_online(i) && (i != self)) {
			register unsigned long msecs = 3000;

			while (atomic_read(&per_cpu(nmi_processors, i)) &&
			       msecs) {
				mdelay(1);
				msecs--;
			}

			if (!msecs) {
				failed++;
				dbg_pr("Processor %i did start\n", (int)i);
			}
		}
	}
	return (unsigned long)failed;
#else
	return 0;
#endif
}

unsigned long breakpoint_active(unsigned long exception, dbg_regs *dbgframe,
				unsigned long processor)
{
	register int i;

	switch (exception) {
	case DEBUGGER_EXCEPTION:
		for (i = 0; i < 4; i++) {
			if (per_cpu(current_dr6, processor) & (1 << i)) {
				if (break_reserved[i])
					return 1;
				else
					return 0;
			}
		}
		break;

	default:
		break;
	}
	return -1;
}

unsigned long enter_debugger(unsigned long exception,
			     dbg_regs *dbgframe,
			     unsigned long processor)
{
	if (debug_lock(&debuglock, &debug_mutex, processor)) {
		if (!breakpoint_active(exception, dbgframe, processor)) {
			debug_unlock(&debuglock, &debug_mutex, processor);

			/* we can only get here if a debugger exception */
			dbgframe->t_flags &= ~SINGLE_STEP;
			dbgframe->t_flags |= RESUME;

			mdb_watchdogs();
			return 0;
		}

		/*  if the processors were already held in the debugger
		 *  due to a trace, ssb, or proceed session on a focus
		 *  processor, do not nest an xcall NMI or signal (not
		 *  if you can help it).
		 */
		if (!atomic_read(&per_cpu(trace_processors, processor)))
			stop_processors_excl_self(processor);
#if IS_ENABLED(CONFIG_VT)
		con_debug_enter(vc_cons[fg_console].d);
#endif
		debugger_command_entry(processor, exception, dbgframe);
#if IS_ENABLED(CONFIG_VT)
		con_debug_leave();
#endif

		/*  do not release the processors for active trace, ssb, or
		 *  proceed sessions on a focus processor.
		 */
		if (!atomic_read(&per_cpu(trace_processors, processor))) {
			free_processors_excl_self(processor);
			wait_restart_excl_self(processor);
		}

		debug_unlock(&debuglock, &debug_mutex, processor);
		mdb_watchdogs();
		return 1;
	}
	mdb_watchdogs();
	return 0;
}

unsigned long check_conditional_breakpoint(dbg_regs *dbgframe,
					   unsigned long processor)
{
	register int i = 0;
	unsigned char *cmd;
	unsigned long valid;

	for (i = 0; i < 4; i++) {
		if (per_cpu(current_dr6, processor) & (1 << i)) {
			if (conditional_breakpoint[i]) {
				cmd = (unsigned char *)&break_condition[i][0];
				return eval_expr(dbgframe, &cmd,
						 &valid);
			}
		}
	}
	return 1;
}

void suspend_loop(dbg_regs *dbgframe,
		  unsigned long processor,
		  unsigned long *ret_code)
{
	/* serialize processors */
	smp_mb__before_atomic();
	per_cpu(processor_state, processor) = PROCESSOR_SUSPEND;
	per_cpu(processor_hold, processor) = 0;
	/* serialize processors */
	smp_mb__after_atomic();

	/* processor suspend loop */
	atomic_inc(&per_cpu(nmi_processors, processor));
	while ((per_cpu(processor_state, processor) != PROCESSOR_RESUME) &&
	       (per_cpu(processor_state, processor) != PROCESSOR_SWITCH)) {
		if ((per_cpu(processor_state, processor) == PROCESSOR_RESUME) ||
		    (per_cpu(processor_state, processor) == PROCESSOR_SWITCH))
			break;
		cpu_relax();
		mdb_watchdogs();
	}
	atomic_dec(&per_cpu(nmi_processors, processor));

	if (per_cpu(processor_state, processor) == PROCESSOR_SWITCH)
		*ret_code = enter_debugger(21, dbgframe, processor);
}

unsigned long debugger_entry(unsigned long exception,
			     dbg_regs *dbgframe,
			     unsigned long processor)
{
	unsigned long ret_code = 1;

	atomic_inc(&debugger_active);
	atomic_inc(&per_cpu(debugger_processors, processor));
	per_cpu(processor_state, processor) = PROCESSOR_DEBUG;

#if IS_ENABLED(CONFIG_MDB_DIRECT_MODE)
	dbg_write_dr7(0);  /* disable breakpoints while debugger is running */
	per_cpu(current_dr6, processor) = __this_cpu_read(curr_dr6);
#else
	dbg_write_dr7(0);  /* disable breakpoints while debugger is running */
#endif
	while (1) {
		switch (exception) {
		case 1:/* int 1 debug exception */
			if (current->thread.debugreg6 & DR_STEP)
				current->thread.debugreg6 &= ~DR_STEP;
			if (per_cpu(break_mask, processor)) {
				dbgframe->t_flags &= ~SINGLE_STEP;
				dbgframe->t_flags |= RESUME;
				break;
			}
			if (!check_conditional_breakpoint(dbgframe,
							  processor)) {
				dbgframe->t_flags &= ~SINGLE_STEP;
				dbgframe->t_flags |= RESUME;
				break;
			}
			ret_code = enter_debugger(exception, dbgframe,
						  processor);
			break;

		case 3:/* int 3 breakpoint */
			if (per_cpu(break_mask, processor)) {
				dbgframe->t_flags &= ~SINGLE_STEP;
				dbgframe->t_flags |= RESUME;
				break;
			}
			ret_code = enter_debugger(exception, dbgframe,
						  processor);
			break;

		case 2: /* nmi */
			/* hold processor */
			if (per_cpu(processor_hold, processor)) {
				suspend_loop(dbgframe, processor, &ret_code);
				break;
			}
			ret_code = enter_debugger(exception, dbgframe,
						  processor);
			break;

		default:
			ret_code = enter_debugger(exception, dbgframe,
						  processor);
			break;
		}

		if (per_cpu(processor_hold, processor)) {
			exception = 2;
			continue;
		}
		break;
	}
	load_debug_registers();
	per_cpu(processor_state, processor) = PROCESSOR_ACTIVE;
	mdb_watchdogs();
	atomic_dec(&per_cpu(debugger_processors, processor));
	atomic_dec(&debugger_active);
	return ret_code;
}

#if IS_ENABLED(CONFIG_MDB_DIRECT_MODE)

void clear_debugger_registers(void)
{
	dbg_write_dr0(0);   /* clear out all breakpoints and breakpoint */
	dbg_write_dr1(0);   /* registers DR0-DR7 */
	dbg_write_dr2(0);
	dbg_write_dr3(0);
	dbg_write_dr6(0);
	dbg_write_dr7(0);
}

void initialize_debugger_registers(void)
{
	/* set mode to GLOBAL EXACT */
	current_dr7 = (DR7DEF | GEXACT | LEXACT);
	/* clear out DR0-DR6 */
	dbg_write_dr0(0);
	dbg_write_dr1(0);
	dbg_write_dr2(0);
	dbg_write_dr3(0);
	dbg_write_dr6(0);
	/* set DR7 register */
	dbg_write_dr7(current_dr7);
}

void set_debug_registers(void)
{
	register int i;

	for (i = 0; i < 4; i++) {
		switch (i) {
		case 0:
			if (break_reserved[i]) {
				current_dr7 &= 0xFFF0FFFF;
				current_dr7 |= G0_BIT;
				current_dr7 |=
					((break_type[i] << ((i * 4) + 16)) |
					 (break_length[i] << ((i * 4) + 18)));
			} else {
				current_dr7 &= 0xFFF0FFFF;
				current_dr7 &= ~G0_BIT;
				current_dr7 &= ~L0_BIT;
			}
			dbg_write_dr0(break_points[i]);
			break;

		case 1:
			if (break_reserved[i]) {
				current_dr7 &= 0xFF0FFFFF;
				current_dr7 |= G1_BIT;
				current_dr7 |=
					((break_type[i] << ((i * 4) + 16)) |
					 (break_length[i] << ((i * 4) + 18)));
			} else {
				current_dr7 &= 0xFF0FFFFF;
				current_dr7 &= ~G1_BIT;
				current_dr7 &= ~L1_BIT;
			}
			dbg_write_dr1(break_points[i]);
			break;

		case 2:
			if (break_reserved[i]) {
				current_dr7 &= 0xF0FFFFFF;
				current_dr7 |= G2_BIT;
				current_dr7 |=
					((break_type[i] << ((i * 4) + 16)) |
					 (break_length[i] << ((i * 4) + 18)));
			} else {
				current_dr7 &= 0xF0FFFFFF;
				current_dr7 &= ~G2_BIT;
				current_dr7 &= ~L2_BIT;
			}
			dbg_write_dr2(break_points[i]);
			break;

		case 3:
			if (break_reserved[i]) {
				current_dr7 &= 0x0FFFFFFF;
				current_dr7 |= G3_BIT;
				current_dr7 |=
					((break_type[i] << ((i * 4) + 16)) |
					 (break_length[i] << ((i * 4) + 18)));
			} else {
				current_dr7 &= 0x0FFFFFFF;
				current_dr7 &= ~G3_BIT;
				current_dr7 &= ~L3_BIT;
			}
			dbg_write_dr3(break_points[i]);
			break;
		}
	}
}

void load_debug_registers(void)
{
	register int i;

	dbg_write_dr6(0);
	for (i = 0; i < 4; i++) {
		switch (i) {
		case 0:
			if (break_reserved[i])
				dbg_write_dr0(break_points[i]);
			break;

		case 1:
			if (break_reserved[i])
				dbg_write_dr1(break_points[i]);
			break;

		case 2:
			if (break_reserved[i])
				dbg_write_dr2(break_points[i]);
			break;

		case 3:
			if (break_reserved[i])
				dbg_write_dr3(break_points[i]);
			break;
		}
	}
	dbg_write_dr7(current_dr7);
}

#else

static struct hw_breakpoint {
	unsigned int		cpu_enabled;
	unsigned int		enabled;
	unsigned long		addr;
	int			len;
	int			type;
	struct perf_event	* __percpu *pev;
} breakinfo[HBP_NUM];

static void mdb_overflow_handler(struct perf_event *event,
				 struct perf_sample_data *data,
				 struct pt_regs *regs)
{
	struct task_struct *tsk = current;
	int i;
	struct perf_event *bp;
	int cpu = raw_smp_processor_id();

	for (i = 0; i < HBP_NUM; i++) {
		if (breakinfo[i].enabled)
			tsk->thread.debugreg6 |= (DR_TRAP0 << i);

		bp = *per_cpu_ptr(breakinfo[i].pev, cpu);
		if (bp == event)
			per_cpu(current_dr6, cpu) |= (1 << i);
	}
}

void clear_debugger_registers(void)
{
	int i;

	for (i = 0; i < HBP_NUM; i++) {
		if (breakinfo[i].pev) {
			unregister_wide_hw_breakpoint(breakinfo[i].pev);
			breakinfo[i].pev = NULL;
		}
	}
}

void initialize_debugger_registers(void)
{
	int i, cpu;
	struct perf_event_attr attr;
	struct perf_event **pevent;

	hw_breakpoint_init(&attr);
	attr.bp_addr = (unsigned long)initialize_debugger_registers;
	attr.bp_len = HW_BREAKPOINT_LEN_1;
	attr.bp_type = HW_BREAKPOINT_W;
	attr.disabled = 1;

	for (i = 0; i < HBP_NUM; i++) {
		if (breakinfo[i].pev)
			continue;

		breakinfo[i].pev = register_wide_hw_breakpoint(&attr,
							       NULL,
							       NULL);

		if (IS_ERR((void * __force)breakinfo[i].pev)) {
			breakinfo[i].pev = NULL;
			clear_debugger_registers();
			return;
		}

		for_each_online_cpu(cpu) {
			pevent = per_cpu_ptr(breakinfo[i].pev, cpu);

			pevent[0]->hw.sample_period = 1;
			pevent[0]->overflow_handler = mdb_overflow_handler;

			if (pevent[0]->destroy) {
				pevent[0]->destroy = NULL;
				dbg_release_bp_slot(*pevent);
			}
		}
	}
}

int set_breakpoint(unsigned long breakno)
{
	int cpu;
	int cnt = 0, err = -1;
	struct perf_event **pevent;

	for_each_online_cpu(cpu) {
		cnt++;
		pevent = per_cpu_ptr(breakinfo[breakno].pev, cpu);
		err = dbg_reserve_bp_slot(*pevent);
		if (err)
			goto fail;
	}

	return 0;

fail:
	for_each_online_cpu(cpu) {
		cnt--;
		if (!cnt)
			break;
		pevent = per_cpu_ptr(breakinfo[breakno].pev, cpu);
		dbg_release_bp_slot(*pevent);
	}
	return err;
}

int clear_breakpoint(unsigned long breakno)
{
	struct perf_event **pevent;
	int cpu;

	for_each_online_cpu(cpu) {
		pevent = per_cpu_ptr(breakinfo[breakno].pev, cpu);
		if (dbg_release_bp_slot(*pevent))
			return -1;
	}
	return 0;
}

void set_debug_registers(void)
{
	register int i, err;

	for (i = 0; i < HBP_NUM; i++) {
		if (break_reserved[i]) {
			if (!breakinfo[i].enabled) {
				err = set_breakpoint(i);
				if (!err)
					breakinfo[i].enabled = 1;
				else
					dbg_pr("Set BP (%d) FAILED %d\n",
					       i, err);
			}
		} else {
			if (breakinfo[i].enabled) {
				if (clear_breakpoint(i))
					dbg_pr("Clear BP (%d) FAILED\n",
					       i);
				else
					breakinfo[i].enabled = 0;
			}
		}
	}
}

unsigned long get_bp_type(unsigned long type)
{
	switch (type) {
	case BREAK_EXECUTE:
		return X86_BREAKPOINT_EXECUTE;

	case BREAK_WRITE:
		return X86_BREAKPOINT_WRITE;

	case BREAK_READWRITE:
		return X86_BREAKPOINT_RW;

	default:
		return X86_BREAKPOINT_EXECUTE;
	}
}

unsigned long get_bp_len(unsigned long len)
{
	switch (len) {
	case ONE_BYTE_FIELD:
		return X86_BREAKPOINT_LEN_1;

	case TWO_BYTE_FIELD:
		return X86_BREAKPOINT_LEN_2;

	case FOUR_BYTE_FIELD:
		return X86_BREAKPOINT_LEN_4;

#if IS_ENABLED(CONFIG_X86_64)
	case EIGHT_BYTE_FIELD:
		return X86_BREAKPOINT_LEN_8;
#endif
	default:
		return X86_BREAKPOINT_LEN_1;
	}
}

void load_debug_registers(void)
{
	register int i;
	int cpu = raw_smp_processor_id();

	per_cpu(current_dr6, cpu) = 0;
	for (i = 0; i < HBP_NUM; i++) {
		if (breakinfo[i].enabled) {
			struct perf_event *bp;
			struct arch_hw_breakpoint *info;
			int val;

			bp = *per_cpu_ptr(breakinfo[i].pev, cpu);
			if (!bp->attr.disabled) {
				arch_uninstall_hw_breakpoint(bp);
				bp->attr.disabled = 1;
			}
			info = counter_arch_bp(bp);
			if (bp->attr.disabled == 1) {
				bp->attr.bp_addr = break_points[i];
				bp->attr.bp_len = get_bp_len(break_length[i]);
				bp->attr.bp_type = get_bp_type(break_type[i]);

				info->address = break_points[i];
				info->len = get_bp_len(break_length[i]);
				info->type = get_bp_type(break_type[i]);

				val = arch_install_hw_breakpoint(bp);
				if (!val)
					bp->attr.disabled = 0;
			}
		} else {
			struct perf_event *bp;
			int cpu = raw_smp_processor_id();

			bp = *per_cpu_ptr(breakinfo[i].pev, cpu);
			if (!bp->attr.disabled) {
				arch_uninstall_hw_breakpoint(bp);
				bp->attr.disabled = 1;
			}
		}
	}
	hw_breakpoint_enable();
}

#endif
