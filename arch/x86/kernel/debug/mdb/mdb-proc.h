
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
 *   AUTHOR   :  Jeff V. Merkey
 *   DESCRIP  :  Minimal Linux Debugger
 *
 ***************************************************************************/

#ifndef _MDB_PROC_H
#define _MDB_PROC_H

/* mdb-main.c */
void mdb_watchdogs(void);

/* mdb-base.c */
extern unsigned long needs_proceed;
extern unsigned long jmp_active;
extern unsigned long trap_disable;
extern unsigned long general_toggle;
extern unsigned long line_info_toggle;
extern unsigned long control_toggle;
extern unsigned long segment_toggle;
extern unsigned long numeric_toggle;
extern unsigned long reason_toggle;
extern unsigned long lockup_toggle;
extern unsigned long user_toggle;
extern unsigned long toggle_user_break;

extern unsigned char *ia_flags[];
extern unsigned char *break_description[];
extern unsigned char *break_length_description[];
extern unsigned char *exception_description[];

/* mdb-ia.c */
extern atomic_t focus_active;
extern atomic_t debugger_active;
extern atomic_t debugger_processors;
extern atomic_t nmi_processors;
extern atomic_t trace_processors;
extern unsigned long processor_hold;
extern unsigned long processor_state;
extern unsigned char *proc_state[];
extern unsigned char *last_dump;
extern unsigned char *last_link;
extern unsigned long last_unasm;
extern unsigned long display_length;
extern unsigned long last_cmd;
extern unsigned long last_cmd_key;
extern unsigned char last_debug_command[100];
extern unsigned long last_display;
extern unsigned char debug_command[100];
extern unsigned long next_unasm;
extern unsigned long pic_1_value;
extern unsigned long pic_2_value;
extern unsigned long break_reserved[4];
extern unsigned long break_points[4];
extern unsigned long break_type[4];
extern unsigned long break_length[4];
extern unsigned long break_temp[4];
extern unsigned long break_go[4];
extern unsigned long break_proceed[4];
extern unsigned long conditional_breakpoint[4];
extern unsigned char break_condition[4][256];
extern dbg_regs last_dbg_regs;
extern unsigned long last_cr0;
extern unsigned long last_cr2;
extern unsigned long last_cr4;
extern unsigned long current_dr7;
extern unsigned long current_dr6;
extern dbg_regs current_dbg_regs;
extern unsigned long break_mask;
extern unsigned long repeat_command;
extern unsigned long total_lines;
extern unsigned long debugger_initialized;
extern unsigned long ssbmode;
extern int nextline;
extern unsigned char *category_strings[13];
extern unsigned long reason_toggle;
extern struct timer_list debug_timer;
#if IS_ENABLED(CONFIG_MDB_DIRECT_MODE)
extern int disable_hw_bp_interface;
#endif

#if IS_ENABLED(CONFIG_SMP) && !IS_ENABLED(CONFIG_X86_ELAN)
unsigned int native_io_apic_read(unsigned int apic, unsigned int reg);
extern int nr_ioapics;
#endif

void initialize_debugger(void);
void clear_debugger_state(void);
void show_debugger_accelerators(void);
void touch_hardlockup_watchdog(void);
void read_dbg_regs(void *, dbg_regs *, unsigned long);
void write_dbg_regs(void *, dbg_regs *, unsigned long);
unsigned long is_accelerator(unsigned long);
unsigned long accel_routine(unsigned long key, void *p);
void display_registers(dbg_regs *, unsigned long);

unsigned long disassemble(dbg_regs *dbgframe, unsigned long p,
			  unsigned long count, unsigned long use,
			  unsigned long type);
void clear_debugger_state(void);
void display_mtrr_registers(void);
void display_gdt(unsigned char *GDT_ADDRESS);
void display_idt(unsigned char *IDT_ADDRESS);
void set_debug_registers(void);
void load_debug_registers(void);
void clear_temp_breakpoints(void);
unsigned long valid_breakpoint(unsigned long address);
unsigned char *dump(unsigned char *p, unsigned long count,
		    unsigned long physical);
unsigned char *dump_word(unsigned char *p, unsigned long count,
			 unsigned long physical);
unsigned char *dump_double(unsigned char *p, unsigned long count,
			   unsigned long physical);
unsigned char *dump_linked_list(unsigned char *p, unsigned long count,
				unsigned long offset);
unsigned char *dump_double_stack(dbg_regs *dbgframe,
				 unsigned char *p,
				 unsigned long count);
unsigned char *dump_local_stack(dbg_regs *dbgframe,
				unsigned char *p,
				unsigned long count);
unsigned long dump_backtrace(unsigned char *p, unsigned long count);
unsigned long debugger_setup(unsigned long processor,
			     unsigned long exception,
			     dbg_regs *dbgframe,
			     unsigned char *panic_msg);
unsigned long debugger_entry(unsigned long exception,
			     dbg_regs *dbgframe,
			     unsigned long processor);
unsigned long debugger_command_entry(unsigned long processor,
				     unsigned long exception,
				     dbg_regs *dbgframe);
unsigned long console_display_reason(dbg_regs *dbgframe,
				     unsigned long reason,
				     unsigned long processor,
				     unsigned long last_cmd);
u64 eval_expr(dbg_regs *dbgframe,
	      unsigned char **p,
	      unsigned long *type);
u64 eval_numeric_expr(dbg_regs *dbgframe,
		      unsigned char **p,
		      unsigned long *type);
u64 eval_disasm_expr(dbg_regs *dbgframe,
		     unsigned char **p,
		     unsigned long *type,
		     int sizeflag,
		     unsigned char **result);
unsigned long unassemble(dbg_regs *dbgframe, unsigned long ip,
			 unsigned long use, unsigned long *ret,
			 unsigned long type);
void display_ascii_table(void);
unsigned char *upcase_string(unsigned char *);
unsigned long validate_address(unsigned long addr);
unsigned long in_keyboard(unsigned char *buffer,
			  unsigned long start,
			  unsigned long length);

unsigned long get_ip(dbg_regs *);
unsigned long get_stack_address(dbg_regs *);
unsigned long get_stack_segment(dbg_regs *);
unsigned short read_memory(void *, void *, unsigned int);
unsigned long ssb_update(dbg_regs *dbgframe, unsigned long processor);
void mdb_breakpoint(void);

#endif
