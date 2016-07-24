
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

#ifndef _MDB_BASE_H
#define _MDB_BASE_H

extern unsigned long line_info_toggle;
extern unsigned char *last_link;

void bt_stack(struct task_struct *task, struct pt_regs *regs,
	      unsigned long *stack);
void machine_emergency_restart(void);
void dump_os_symbol_table_match(unsigned char *);
void clear_screen(void);
int mdb_getkey(void);

unsigned long enter_key_acc(unsigned long key,
			    void *dbgframe,
			    accel_key *accel);
unsigned long activate_register_display_acc(unsigned long key,
					    void *dbgframe,
					    accel_key *accel);

unsigned long display_debugger_help_help(unsigned char *command_line,
					 dbg_parser *parser);
unsigned long display_debugger_help(unsigned char *command_line,
				    dbg_regs *dbgframe,
				    unsigned long exception,
				    dbg_parser *parser);

unsigned long asc_table_help(unsigned char *command_line,
			     dbg_parser *parser);
unsigned long display_asc_table(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);

unsigned long display_toggle_help(unsigned char *command_line,
				  dbg_parser *parser);
unsigned long display_toggle_all(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);
unsigned long process_tu_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_tb_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_td_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_tl_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_tg_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_tc_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_tn_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_tr_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_toggle_user(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long process_ts_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_ta_toggle(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);

unsigned long display_debugger_version_help(unsigned char *command_line,
					    dbg_parser *parser);
unsigned long display_debugger_version(unsigned char *cmd,
				       dbg_regs *dbgframe,
				       unsigned long exception,
				       dbg_parser *parser);

unsigned long display_kprocess_help(unsigned char *command_line,
				    dbg_parser *parser);
unsigned long display_kprocess(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);

unsigned long display_kernel_queue_help(unsigned char *command_line,
					dbg_parser *parser);
unsigned long display_kernel_queue(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser);

unsigned long display_symbols_help(unsigned char *command_line,
				   dbg_parser *parser);
unsigned long display_symbols(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);

unsigned long display_loader_map_help(unsigned char *command_line,
				      dbg_parser *parser);
unsigned long display_loader_map(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long display_module_help(unsigned char *command_line,
				  dbg_parser *parser);
unsigned long display_module_info(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_processes_help(unsigned char *command_line,
				     dbg_parser *parser);
unsigned long display_processes(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);

unsigned long display_registers_help(unsigned char *command_line,
				     dbg_parser *parser);
unsigned long control_registers(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long display_all_registers(unsigned char *cmd,
				    dbg_regs *dbgframe,
				    unsigned long exception,
				    dbg_parser *parser);
unsigned long segment_registers(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long display_numeric_registers(unsigned char *cmd,
					dbg_regs *dbgframe,
					unsigned long exception,
					dbg_parser *parser);
unsigned long general_registers(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long display_default_registers(unsigned char *cmd,
					dbg_regs *dbgframe,
					unsigned long exception,
					dbg_parser *parser);

unsigned long display_apic_help(unsigned char *command_line,
				dbg_parser *parser);
unsigned long display_apic_info(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);

unsigned long list_processors(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long list_processor_frame(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser);

unsigned long reason_help(unsigned char *command_line,
			  dbg_parser *parser);
unsigned long reason_display(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_m_p_s_help(unsigned char *command_line,
				 dbg_parser *parser);
unsigned long display_m_pS(unsigned char *cmd,
			   dbg_regs *dbgframe,
			   unsigned long exception,
			   dbg_parser *parser);

unsigned long clear_screen_help(unsigned char *command_line,
				dbg_parser *parser);
unsigned long clear_debugger_screen(unsigned char *cmd,
				    dbg_regs *dbgframe,
				    unsigned long exception,
				    dbg_parser *parser);

unsigned long search_memory_help(unsigned char *command_line,
				 dbg_parser *parser);
unsigned long search_memory(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser);
unsigned long search_memory_b(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long search_memory_w(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long search_memory_d(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long search_memory_q(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);

unsigned long change_memory_help(unsigned char *command_line,
				 dbg_parser *parser);
unsigned long change_word_value(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long change_double_value(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long change_quad_value(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long change_byte_value(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long change_default_value(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser);

unsigned long display_close_help(unsigned char *command_line,
				 dbg_parser *parser);
unsigned long display_close_symbols(unsigned char *cmd,
				    dbg_regs *dbgframe,
				    unsigned long exception,
				    dbg_parser *parser);

unsigned long display_i_nt_r_help(unsigned char *command_line,
				  dbg_parser *parser);
unsigned long display_interrupt_table(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser);

unsigned long view_screens_help(unsigned char *command_line,
				dbg_parser *parser);
unsigned long display_screen_list(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_ioapic_help(unsigned char *command_line,
				  dbg_parser *parser);
unsigned long display_ioapic_info(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_dump_help(unsigned char *command_line,
				dbg_parser *parser);
unsigned long debugger_walk_stack(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long debugger_dump_linked_list(unsigned char *cmd,
					dbg_regs *dbgframe,
					unsigned long exception,
					dbg_parser *parser);
unsigned long debugger_dump_stack(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long debugger_dump_double_stack(unsigned char *cmd,
					 dbg_regs *dbgframe,
					 unsigned long exception,
					 dbg_parser *parser);
unsigned long debugger_dump_quad_stack(unsigned char *cmd,
				       dbg_regs *dbgframe,
				       unsigned long exception,
				       dbg_parser *parser);

unsigned long debugger_dump_quad(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);
unsigned long debugger_dump_double(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser);
unsigned long debugger_dump_word(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);
unsigned long debugger_dump_byte(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long debugger_dump_quad_phys(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser);
unsigned long debugger_dump_double_phys(unsigned char *cmd,
					dbg_regs *dbgframe,
					unsigned long exception,
					dbg_parser *parser);
unsigned long debugger_dump_word_phys(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser);
unsigned long debugger_dump_byte_phys(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser);

unsigned long display_disassemble_help(unsigned char *command_line,
				       dbg_parser *parser);
unsigned long process_disasm_16(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_disasm_32(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_disassemble_any(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser);
unsigned long process_disassemble_att(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser);

unsigned long reboot_system_help(unsigned char *command_line,
				 dbg_parser *parser);
unsigned long reboot_system(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser);

unsigned long display_sections_help(unsigned char *command_line,
				    dbg_parser *parser);
unsigned long display_sections(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long display_kprocess_help(unsigned char *command_line,
				    dbg_parser *parser);
unsigned long display_kprocess(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long display_processor_status_help(unsigned char *command_line,
					    dbg_parser *parser);
unsigned long display_processor_status(unsigned char *cmd,
				       dbg_regs *dbgframe,
				       unsigned long exception,
				       dbg_parser *parser);

unsigned long back_trace_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long back_trace_all_pid(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);
unsigned long back_trace_pid(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);
unsigned long back_trace_stack(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long timer_break_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long timer_breakpoint(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long timer_del_break(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);

unsigned long display_process_switch_help(unsigned char *command_line,
					  dbg_parser *parser);
unsigned long switch_kprocess(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);

unsigned long percpu_help(unsigned char *command_line,
			  dbg_parser *parser);
unsigned long dump_per_cpu(unsigned char *cmd,
			   dbg_regs *dbgframe,
			   unsigned long exception,
			   dbg_parser *parser);

#if IS_ENABLED(CONFIG_MODULES)
unsigned long list_modules_help(unsigned char *command_line,
				dbg_parser *parser);
unsigned long list_modules(unsigned char *cmd,
			   dbg_regs *dbgframe,
			   unsigned long exception,
			   dbg_parser *parser);
unsigned long unload_module(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser);
#endif

#endif
