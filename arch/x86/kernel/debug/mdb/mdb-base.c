
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
#include <asm/segment.h>
#include <linux/atomic.h>
#include <asm/msr.h>
#include <linux/io.h>

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

unsigned long debug_deref;
unsigned long full_deref_toggle = 1;
unsigned long control_toggle;
unsigned long numeric_toggle;
unsigned long user_toggle = 1;
unsigned long general_toggle = 1;
unsigned long line_info_toggle = 1;
unsigned long segment_toggle = 1;
unsigned long reason_toggle = 1;
unsigned long lockup_toggle = 1;
unsigned long toggle_user_break = 1;

unsigned char *check_commands[] = {
	"W",
	"D",
	"DB",
	"DW",
	"DD",
	"DDS",
	"DQ",
	"DQS",
	"DS",
	"DL",
	"U",
	"UU",
	"S",
	"SS",
	"DP",
	"DPW",
	"DPD",
	"DPQ",
	"DPB",
	"SSB",
	"ID",
	"UX",
};

unsigned long enter_key_acc(unsigned long key, void *dbgframe,
			    accel_key *accel)
{
	unsigned char *verb_buffer = &workbuf[0];

	register unsigned char *verb, *pp, *vp;
	register unsigned long count;

	if (!debug_command[0]) {
		count = 0;
		pp = (unsigned char *)last_debug_command;
		vp = &verb_buffer[0];
		verb = &verb_buffer[0];
		while (*pp && *pp == ' ' && count++ < 80)
			pp++;

		while (*pp && *pp != ' ' && count++ < 80)
			*vp++ = *pp++;
		*vp = '\0';

		while (*pp && *pp == ' ' && count++ < 80)
			pp++;

		upcase_string(verb);
		if (!strcmp(verb, "P") || (last_cmd == K_F8)) {
			strcpy((char *)debug_command, "P");
		} else if (!strcmp(verb, "T") || (last_cmd == K_F7)) {
			strcpy((char *)debug_command, "T");
		} else if (!strcmp(verb, "W")  || !strcmp(verb, "D")   ||
			   !strcmp(verb, "DB") || !strcmp(verb, "DW")  ||
			   !strcmp(verb, "DD") || !strcmp(verb, "DDS") ||
			   !strcmp(verb, "DQ") || !strcmp(verb, "DQS") ||
			   !strcmp(verb, "DS") || !strcmp(verb, "DL")  ||
			   !strcmp(verb, "U")  || !strcmp(verb, "UU")  ||
			   !strcmp(verb, "S")  || !strcmp(verb, "SS")  ||
			   !strcmp(verb, "DP") || !strcmp(verb, "DPW")  ||
			   !strcmp(verb, "DPD") || !strcmp(verb, "DPQ")  ||
			   !strcmp(verb, "DPB") || !strcmp(verb, "SSB") ||
			   !strcmp(verb, "ID")  || !strcmp(verb, "UX")) {
			strcpy((char *)debug_command, verb);
			repeat_command = 1;
		}
	}
	return 0;
}

unsigned long display_debugger_help_help(unsigned char *command_line,
					 dbg_parser *parser)
{
	dbg_pr("HELP         <enter>  - list all commands\n");
	dbg_pr("HELP command <enter>  - help for a specific command\n");

	return 1;
}

unsigned long display_debugger_help(unsigned char *command_line,
				    dbg_regs *dbgframe,
				    unsigned long exception,
				    dbg_parser *parser)
{
	register unsigned long count;
	unsigned char *verb_buffer = &workbuf[0];
	register unsigned char *verb, *pp, *vp;

	command_line = &command_line[parser->length];
	while (*command_line && *command_line == ' ')
		command_line++;

	count = 0;
	pp = command_line;
	vp = &verb_buffer[0];
	verb = &verb_buffer[0];
	while (*pp && *pp == ' ' && count++ < 80)
		pp++;

	while (*pp && *pp != ' ' && count++ < 80)
		*vp++ = *pp++;
	*vp = '\0';

	while (*pp && *pp == ' ' && count++ < 80)
		pp++;

	debugger_parser_help_routine(verb, command_line);
	return 1;
}

/* TIMER */

struct timer_list debug_timer;

void debug_timer_callback(void)
{
	debug_timer.expires = jiffies + (HZ * debug_timer.data);
	debug_timer.function = (void (*)(unsigned long))debug_timer_callback;
	add_timer(&debug_timer);
	mdb_breakpoint();
}

unsigned long timer_break_help(unsigned char *command_line,
			       dbg_parser *parser)
{
	dbg_pr("addtimer <seconds>      - add int3 timer\n");
	dbg_pr("deltimer                - del int3 timer\n");
	return 1;
}

unsigned long timer_breakpoint(unsigned char *cmd, dbg_regs *dbgframe,
			       unsigned long exception, dbg_parser *parser)
{
	register int seconds;
	unsigned long valid = 0;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (debug_timer.data) {
		dbg_pr("debug timer is already active.  seconds = %i\n",
		       (int)debug_timer.data);
		return 1;
	}

	seconds = eval_numeric_expr(dbgframe, &cmd, &valid);
	if (valid) {
		init_timer(&debug_timer);
		debug_timer.data = seconds;
		debug_timer.expires = jiffies + (HZ * seconds);
		debug_timer.function =
			(void (*)(unsigned long))debug_timer_callback;
		add_timer(&debug_timer);

		dbg_pr("debug timer created.  seconds = %i\n", seconds);
	}
	return 1;
}

unsigned long timer_del_break(unsigned char *cmd, dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	del_timer(&debug_timer);
	debug_timer.data = 0;
	dbg_pr("debug timer deleted\n");
	return 1;
}

/* PERCPU */

unsigned long percpu_help(unsigned char *command_line, dbg_parser *parser)
{
	dbg_pr("percpu <symbol> <cpu> <len> - display percpu variable\n");
	return 1;
}

unsigned long dump_per_cpu(unsigned char *cmd, dbg_regs *dbgframe,
			   unsigned long exception,
			   dbg_parser *parser)
{
	unsigned long address, addr, err;
	unsigned long cpu, val, valid, p, len = 0;

#if defined(__per_cpu_offset)
#define MDB_PCU(cpu) __per_cpu_offset(cpu)
#else
#if IS_ENABLED(CONFIG_SMP)
#define MDB_PCU(cpu) __per_cpu_offset[cpu]
#else
#define MDB_PCU(cpu) 0
#endif
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		dbg_pr("invalid percpu symbol or address\n");
		return 1;
	}

	cpu = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		if (!cpu_online(cpu)) {
			dbg_pr("cpu %ld is not online\n", cpu);
			return 1;
		}

		len = eval_expr(dbgframe, &cmd, &valid);
		switch (len) {
		case 1:
		case 2:
		case 4:
		case 8:
			break;

		default:
#if IS_ENABLED(CONFIG_X86_64)
			len = 8;
#else
			len = 4;
#endif
			break;
		}

		for_each_online_cpu(p) {
			if (cpu != p)
				continue;
			addr = address + MDB_PCU(p);
			err = mdb_getlword((u64 *)&val, addr, len);
			if (err)
				continue;
			if (dbg_pr("0x%p  cpu %d:  %0llX  (len %ld)\n", addr,
				   p, val, len))
				return 1;
		}
	} else {
#if IS_ENABLED(CONFIG_X86_64)
		len = 8;
#else
		len = 4;
#endif
		for_each_online_cpu(p) {
			addr = address + MDB_PCU(p);
			err = mdb_getlword((u64 *)&val, addr, len);
			if (err)
				continue;

			if (dbg_pr("0x%p  cpu %d:  %0llX  (len %ld)\n", addr,
				   p, val, len))
				return 1;
		}
	}
#undef MDB_PCU
	return 1;
}

/* BT, BTA, BTP */

unsigned long back_trace_help(unsigned char *command_line, dbg_parser *parser)
{
	dbg_pr("bt <addr>                - display backtrace\n");
	dbg_pr("bta                      - display backtrace all pids\n");
	dbg_pr("btp <pid>                - display backtrace by pid\n");
	return 1;
}

unsigned long back_trace_all_pid(unsigned char *cmd, dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	struct task_struct *p, *g;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	do_each_thread(g, p) {
		if (p) {
			if (dbg_pr("Stack backtrace for pid %d\n", p->pid))
				return 1;
			bt_stack(p, NULL, NULL);
		}
	} while_each_thread(g, p);
	return 1;
}

unsigned long back_trace_pid(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser)
{
	int pid;
	unsigned long valid = 0;
	struct task_struct *p, *g;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	pid = eval_numeric_expr(dbgframe, &cmd, &valid);
	if (valid) {
		do_each_thread(g, p) {
			if (p && (p->pid == pid)) {
				dbg_pr("Stack backtrace for pid %d\n",
				       p->pid);
				bt_stack(p, NULL, NULL);
				return 1;
			}
		} while_each_thread(g, p);
		dbg_pr("No process with pid %d found\n", pid);
	} else {
		dbg_pr("invalid pid entered for backtrace\n");
	}

	return 1;
}

unsigned long back_trace_stack(unsigned char *cmd, dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser)
{
	unsigned long valid = 0, address;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		dbg_pr("Stack backtrace for address 0x%p\n",
		       (unsigned int *)address);
		bt_stack(NULL, NULL, (unsigned long *)address);
		return 1;
	}
	dbg_pr("Stack backtrace for address 0x%p\n",
	       (unsigned int *)get_stack_address(dbgframe));
	bt_stack(NULL, NULL,
		 (unsigned long *)get_stack_address(dbgframe));
	return 1;
}

#define ASC_NULL "| %3i | (0x%02X) | (%1i%1i%1i%1i%1i%1i%1i%1ib) | NULL  |"
#define ASC_BKSP "| %3i | (0x%02X) | (%1i%1i%1i%1i%1i%1i%1i%1ib) | BKSP  |"
#define ASC_TAB  "| %3i | (0x%02X) | (%1i%1i%1i%1i%1i%1i%1i%1ib) | TAB   |"
#define ASC_CR   "| %3i | (0x%02X) | (%1i%1i%1i%1i%1i%1i%1i%1ib) | <CR>  |"
#define ASC_LF   "| %3i | (0x%02X) | (%1i%1i%1i%1i%1i%1i%1i%1ib) | <LF>  |"
#define ASC_SP   "| %3i | (0x%02X) | (%1i%1i%1i%1i%1i%1i%1i%1ib) | SPACE |"
#define ASC_BASE "| %3i | (0x%02X) | (%1i%1i%1i%1i%1i%1i%1i%1ib) |  %c    |"

void display_ascii_table(void)
{
	register unsigned long i;
	union bhex {
		unsigned int i;
		struct btemp {
			unsigned one : 1;
			unsigned two : 1;
			unsigned three : 1;
			unsigned four : 1;
			unsigned five : 1;
			unsigned six : 1;
			unsigned seven : 1;
			unsigned eight : 1;
		} b;
	} val;

	dbg_pr("ASCII Table\n");
	for (i = 0; i < 256; i++) {
		val.i = i;
		switch (i) {
		case 0:
			if (dbg_pr(ASC_NULL,
				   (int)i, i,
				   (int)val.b.eight, (int)val.b.seven,
				   (int)val.b.six,
				   (int)val.b.five,  (int)val.b.four,
				   (int)val.b.three,
				   (int)val.b.two, (int)val.b.one))
				return;
			break;

		case 8:
			if (dbg_pr(ASC_BKSP,
				   (int)i, i,
				   (int)val.b.eight, (int)val.b.seven,
				   (int)val.b.six,
				   (int)val.b.five,  (int)val.b.four,
				   (int)val.b.three,
				   (int)val.b.two, (int)val.b.one))
				return;
			break;

		case 9:
			if (dbg_pr(ASC_TAB,
				   (int)i, i,
				   (int)val.b.eight, (int)val.b.seven,
				   (int)val.b.six,
				   (int)val.b.five,  (int)val.b.four,
				   (int)val.b.three,
				   (int)val.b.two, (int)val.b.one))
				return;
			break;

		case 10:
			if (dbg_pr(ASC_CR,
				   (int)i, i,
				   (int)val.b.eight, (int)val.b.seven,
				   (int)val.b.six,
				   (int)val.b.five,  (int)val.b.four,
				   (int)val.b.three,
				   (int)val.b.two, (int)val.b.one))
				return;
			break;

		case 13:
			if (dbg_pr(ASC_LF,
				   (int)i, i,
				   (int)val.b.eight, (int)val.b.seven,
				   (int)val.b.six,
				   (int)val.b.five,  (int)val.b.four,
				   (int)val.b.three,
				   (int)val.b.two, (int)val.b.one))
				return;
			break;

		case 32:
			if (dbg_pr(ASC_SP,
				   (int)i, i,
				   (int)val.b.eight, (int)val.b.seven,
				   (int)val.b.six,
				   (int)val.b.five,  (int)val.b.four,
				   (int)val.b.three,
				   (int)val.b.two, (int)val.b.one))
				return;
			break;

		default:
			if (dbg_pr(ASC_BASE,
				   (int)i, i,
				   (int)val.b.eight, (int)val.b.seven,
				   (int)val.b.six,
				   (int)val.b.five,  (int)val.b.four,
				   (int)val.b.three,
				   (int)val.b.two, (int)val.b.one,
				   (unsigned char)i))
				return;
			break;
		}
		if (dbg_pr("\n"))
			return;
	}
}

#if IS_ENABLED(CONFIG_MODULES)

/* LSMOD, .M */

unsigned long list_modules_help(unsigned char *command_line,
				dbg_parser *parser)
{
	dbg_pr(".M                       - list loaded modules\n");
	dbg_pr("lsmod                    - list loaded modules\n");
	dbg_pr("rmmod <name>             - unload module\n");
	return 1;
}

unsigned long list_modules(unsigned char *cmd, dbg_regs *dbgframe,
			   unsigned long exception,
			   dbg_parser *parser)
{
	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (*cmd)
		mdb_modules(cmd, dbg_pr);
	else
		mdb_modules(NULL, dbg_pr);
	return 1;
}

unsigned long unload_module(unsigned char *cmd, dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser)
{
	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	dbg_pr("Module unload unsupported in this version\n");
	return 0;
}

#endif

/* REBOOT */

unsigned long reboot_system_help(unsigned char *command_line,
				 dbg_parser *parser)
{
	dbg_pr("reboot                   - reboot host system\n");
	dbg_pr("reboot force             - reboot from current processor\n");
	return 1;
}

static inline int mdb_strnicmp(const unsigned char *s1,
			       const unsigned char *s2,
			       int len)
{
	while (len--) {
		if (tolower(*s1++) != tolower(*s2++))
			return 1;
	}
	return 0;
}

unsigned long reboot_system(unsigned char *cmd, dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser)
{
	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (!mdb_strnicmp(cmd, "force", 5)) {
		machine_emergency_restart();
		return 1;
	}

	if (!get_processor_id())
		machine_emergency_restart();
	else
		dbg_pr("not on processor 0.  try 'reboot force'\n");
	return 1;
}

/* SECTIONS, .S */

unsigned long display_sections_help(unsigned char *command_line,
				    dbg_parser *parser)
{
	dbg_pr("sections                 - display module sections\n");
	dbg_pr(".s                       - display module sections\n");
	return 1;
}

unsigned long display_sections(unsigned char *cmd,
			       dbg_regs *dbgframe, unsigned long exception,
			       dbg_parser *parser)
{
	dbg_pr("\n");
	return 1;
}

/* PSW */

static inline int mdb_process_cpu(const struct task_struct *p)
{
	unsigned int cpu = task_thread_info(p)->cpu;

	if (cpu > num_possible_cpus())
		cpu = 0;
	return cpu;
}

unsigned long display_process_switch_help(unsigned char *command_line,
					  dbg_parser *parser)
{
	dbg_pr("pid <pid>                - switch to another process\n");
	return 1;
}

/* PS, .P */

int display_pid_header(void)
{
	return (dbg_pr("%-*s      Pid   Parent[*] State %-*sCPU Command\n",
		       (int)(2 * sizeof(void *)) + 2, "Task Addr",
		       (int)(2 * sizeof(void *)) + 2, "Thread"));
}

int display_pid(struct task_struct *p)
{
	return (dbg_pr("0x%p %8d %8d  %d    %c  0x%p %02u %c%s\n",
		       (void *)p,
		       p->pid,
		       (mdb_verify_rw(p->real_parent, 4) ?
			0 : p->real_parent->pid),
		       task_curr(p),
		       (p->state == 0) ? 'R' :
		       (p->state < 0) ? 'U' :
		       (p->state & TASK_UNINTERRUPTIBLE) ? 'D' :
		       (p->state & TASK_STOPPED) ? 'T' :
		       (p->state & TASK_TRACED) ? 'C' :
		       (p->exit_state & EXIT_ZOMBIE) ?  'Z' :
		       (p->exit_state & EXIT_DEAD) ? 'E' :
		       (p->state & TASK_INTERRUPTIBLE) ? 'S' :
		       (!p->mm &&
			(p->state & TASK_INTERRUPTIBLE)) ? 'M' : '?',
		       (void *)(&p->thread), mdb_process_cpu(p),
		       (p == curr_task(get_processor_id())) ? '*' : ' ',
		       p->comm));
}

unsigned long display_kprocess_help(unsigned char *command_line,
				    dbg_parser *parser)
{
	dbg_pr("ps <pid>                 - display kernel processes\n");
	dbg_pr(".p <pid>                 - display kernel processes\n");
	return 1;
}

unsigned long display_kprocess(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser)
{
	int pid;
	struct task_struct *p, *g;
	unsigned long valid = 0;
	dbg_regs *search_frame, task_frame;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	pid = eval_numeric_expr(dbgframe, &cmd, &valid);
	if (valid) {
		do_each_thread(g, p) {
			if (p && (p->pid == pid)) {
				if (display_pid_header())
					return 1;

				if (display_pid(p))
					return 1;

				search_frame = &task_frame;
				read_task_frame(search_frame, p);
				display_tss(search_frame);
#if IS_ENABLED(CONFIG_X86_32)
				disassemble(search_frame, search_frame->t_ip, 1,
					    -1, 0);
#endif
				return 1;
			}
		} while_each_thread(g, p);
		dbg_pr("invalid task pid\n");
		return 1;
	}

	if (display_pid_header())
		return 1;

	do_each_thread(g, p) {
		if (p) {
			if (display_pid(p))
				return 1;
		}
	} while_each_thread(g, p);
	return 1;
}

/* A */

unsigned long asc_table_help(unsigned char *command_line, dbg_parser *parser)
{
	dbg_pr("a                        - display ASCII Table\n");
	return 1;
}

unsigned long display_asc_table(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	display_ascii_table();
	return 1;
}

unsigned long disassemble(dbg_regs *dbgframe, unsigned long p,
			  unsigned long count, unsigned long use,
			  unsigned long type)
{
	register unsigned long i;
	unsigned char *symbol_name;
	unsigned char *module_name;

	for (i = 0; i < count; i++) {
		symbol_name = get_symbol_value(p, &symbuf[0], MAX_SYMBOL_LEN);
		if (symbol_name) {
			i++;
			module_name =
				get_module_symbol_value(p,
							&modbuf[0],
							MAX_SYMBOL_LEN);
			if (module_name) {
				if (dbg_pr("%s|%s:\n", module_name,
					   symbol_name))
					return p;
			} else
				if (dbg_pr("%s:\n", symbol_name))
					return p;
		}
		if (i >= count && count != 1)
			break;

		if (unassemble(dbgframe, p, use, &p, type))
			return p;
	}

	return p;
}

unsigned long search_results(unsigned char *p, unsigned long count)
{
	unsigned char *symbol_name;
	unsigned char *module_name;
	register unsigned long i, r, total;
	unsigned char ch;

	for (r = 0; r < count; r++) {
		symbol_name = get_symbol_value((unsigned long)p, &symbuf[0],
					       MAX_SYMBOL_LEN);
		if (symbol_name) {
			module_name =
				get_module_symbol_value((unsigned long)p,
							&modbuf[0],
							MAX_SYMBOL_LEN);
			if (module_name) {
				if (dbg_pr("%s|%s:\n", module_name,
					   symbol_name))
					return 1;
			} else
				if (dbg_pr("%s:\n", symbol_name))
					return 1;
			if (r++ >= count && count != 1)
				break;
		}
		dbg_pr("%p ", p);
		for (total = 0, i = 0; i < 16; i++, total++)
			dbg_pr("%02X",
			       mdb_getword((unsigned long)&p[i],
					   1));
		dbg_pr("  ");
		for (i = 0; i < total; i++) {
			ch = mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		if (dbg_pr("\n"))
			return 1;

		p = (void *)((unsigned long)p + (unsigned long)total);
	}
	return 0;
}

unsigned char *dump(unsigned char *p, unsigned long count,
		    unsigned long physical)
{
	unsigned char *symbol_name;
	unsigned char *module_name;
	register unsigned long i, r, total;
	unsigned char ch;

	for (r = 0; r < count; r++) {
		symbol_name = get_symbol_value((unsigned long)p,
					       &symbuf[0],
					       MAX_SYMBOL_LEN);
		if (symbol_name) {
			module_name =
				get_module_symbol_value((unsigned long)p,
							&modbuf[0],
							MAX_SYMBOL_LEN);
			if (module_name)
				dbg_pr("%s|%s:\n", module_name, symbol_name);
			else
				dbg_pr("%s:\n", symbol_name);
			if (r++ >= count && count != 1)
				break;
		}

		dbg_pr("%p ", p);
		for (total = 0, i = 0; i < 16; i++, total++) {
			dbg_pr("%02X", physical
			       ? mdb_phys_getword((unsigned long)
						  &p[i], 1)
			       : mdb_getword((unsigned long)
					     &p[i], 1));
		}
		dbg_pr("  ");
		for (i = 0; i < total; i++) {
			ch = physical
				? mdb_phys_getword((unsigned long)&p[i], 1)
				: mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		dbg_pr("\n");

		p = (void *)((unsigned long)p + (unsigned long)total);
	}

	return p;
}

unsigned long dump_word_search(unsigned char *p, unsigned long count)
{
	register int i, r;
	unsigned short *wp;
	unsigned char *symbol_name;
	unsigned char *module_name;
	unsigned char ch;

	wp = (unsigned short *)p;
	for (r = 0; r < count; r++) {
		symbol_name = get_symbol_value((unsigned long)p,
					       &symbuf[0],
					       MAX_SYMBOL_LEN);
		if (symbol_name) {
			module_name =
				get_module_symbol_value((unsigned long)p,
							&modbuf[0],
							MAX_SYMBOL_LEN);
			if (module_name) {
				if (dbg_pr("%s|%s:\n", module_name,
					   symbol_name))
					return 1;
			} else
				if (dbg_pr("%s:\n", symbol_name))
					return 1;
			if (r++ >= count && count != 1)
				break;
		}
		dbg_pr("%p ", p);
		for (i = 0; i < (16 / 2); i++)
			dbg_pr(" %04X", mdb_getword((unsigned long)
						    &wp[i], 2));
		dbg_pr("  ");
		for (i = 0; i < 16; i++) {
			ch = mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		if (dbg_pr("\n"))
			return 1;

		p = (void *)((unsigned long)p + 16);
		wp = (unsigned short *)p;
	}

	return 0;
}

unsigned char *dump_word(unsigned char *p, unsigned long count,
			 unsigned long physical)
{
	register int i, r;
	unsigned short *wp;
	unsigned char *symbol_name;
	unsigned char *module_name;
	unsigned char ch;

	wp = (unsigned short *)p;
	for (r = 0; r < count; r++) {
		symbol_name = get_symbol_value((unsigned long)p,
					       &symbuf[0],
					       MAX_SYMBOL_LEN);
		if (symbol_name) {
			module_name =
				get_module_symbol_value((unsigned long)p,
							&modbuf[0],
							MAX_SYMBOL_LEN);
			if (module_name)
				dbg_pr("%s|%s:\n", module_name, symbol_name);
			else
				dbg_pr("%s:\n", symbol_name);
			if (r++ >= count && count != 1)
				break;
		}
		dbg_pr("%p ", p);
		for (i = 0; i < (16 / 2); i++) {
			dbg_pr(" %04X", physical
			       ? mdb_phys_getword((unsigned long)
						  &wp[i], 2)
			       : mdb_getword((unsigned long)
					     &wp[i], 2));
		}
		dbg_pr("  ");
		for (i = 0; i < 16; i++) {
			ch = physical
				? mdb_phys_getword((unsigned long)&p[i], 1)
				: mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		dbg_pr("\n");

		p = (void *)((unsigned long)p + 16);
		wp = (unsigned short *)p;
	}

	return p;
}

unsigned long dump_double_search(unsigned char *p, unsigned long count)
{
	register int i, r;
	u32 *lp;
	unsigned char *symbol_name;
	unsigned char *module_name;
	unsigned char ch;

	lp = (u32 *)p;

	for (r = 0; r < count; r++) {
		symbol_name = get_symbol_value((unsigned long)p,
					       &symbuf[0],
					       MAX_SYMBOL_LEN);
		if (symbol_name) {
			module_name =
				get_module_symbol_value((unsigned long)p,
							&modbuf[0],
							MAX_SYMBOL_LEN);
			if (module_name) {
				if (dbg_pr("%s|%s:\n", module_name,
					   symbol_name))
					return 1;
			} else
				if (dbg_pr("%s:\n", symbol_name))
					return 1;
			if (r++ >= count && count != 1)
				break;
		}
		dbg_pr("%p ", p);
		for (i = 0; i < (16 / 4); i++)
			dbg_pr(" %08X", mdb_getword((unsigned long)
						    &lp[i], 4));
		dbg_pr("  ");
		for (i = 0; i < 16; i++) {
			ch = mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		if (dbg_pr("\n"))
			return 1;

		p = (void *)((unsigned long)p + 16);
		lp = (u32 *)p;
	}

	return 0;
}

unsigned char *dump_quad(unsigned char *p, unsigned long count,
			 unsigned long physical)
{
	register int i, r;
	u64 *lp;
	unsigned char *symbol_name;
	unsigned char *module_name;
	unsigned char ch;

	lp = (u64 *)p;

	for (r = 0; r < count; r++) {
		symbol_name = get_symbol_value((unsigned long)p,
					       &symbuf[0],
					       MAX_SYMBOL_LEN);
		if (symbol_name) {
			module_name =
				get_module_symbol_value((unsigned long)p,
							&modbuf[0],
							MAX_SYMBOL_LEN);
			if (module_name)
				dbg_pr("%s|%s:\n", module_name, symbol_name);
			else
				dbg_pr("%s:\n", symbol_name);
			if (r++ >= count && count != 1)
				break;
		}
		dbg_pr("%p ", p);
		for (i = 0; i < (16 / 8); i++) {
			dbg_pr(" %016llX", physical
			       ? (u64)mdb_phys_getqword(&lp[i], 8)
			       : (u64)mdb_getqword(&lp[i], 8));
		}
		dbg_pr("  ");
		for (i = 0; i < 16; i++) {
			ch = physical
				? mdb_phys_getword((unsigned long)&p[i], 1)
				: mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		dbg_pr("\n");

		p = (void *)((unsigned long)p + 16);
		lp = (u64 *)p;
	}

	return p;
}

unsigned char *dump_double(unsigned char *p, unsigned long count,
			   unsigned long physical)
{
	register int i, r;
	u32 *lp;
	unsigned char *symbol_name;
	unsigned char *module_name;
	unsigned char ch;

	lp = (u32 *)p;

	for (r = 0; r < count; r++) {
		symbol_name = get_symbol_value((unsigned long)p,
					       &symbuf[0],
					       MAX_SYMBOL_LEN);
		if (symbol_name) {
			module_name =
				get_module_symbol_value((unsigned long)p,
							&modbuf[0],
							MAX_SYMBOL_LEN);
			if (module_name)
				dbg_pr("%s|%s:\n", module_name, symbol_name);
			else
				dbg_pr("%s:\n", symbol_name);
			if (r++ >= count && count != 1)
				break;
		}
		dbg_pr("%p ", p);
		for (i = 0; i < (16 / 4); i++) {
			dbg_pr(" %08X", physical
			       ? mdb_phys_getword((unsigned long)
						  &lp[i], 4)
			       : mdb_getword((unsigned long)
					     &lp[i], 4));
		}
		dbg_pr("  ");
		for (i = 0; i < 16; i++) {
			ch = physical
				? mdb_phys_getword((unsigned long)&p[i], 1)
				: mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		dbg_pr("\n");

		p = (void *)((unsigned long)p + 16);
		lp = (u32 *)p;
	}

	return p;
}

unsigned char *dump_linked_list(unsigned char *p, unsigned long count,
				unsigned long offset)
{
	register int i, r;
	unsigned long *lp;
	unsigned char ch;

	lp = (unsigned long *)p;

	dbg_pr("Linked List ->[%p + %X] = %08X\n", lp, offset,
	       mdb_getword((unsigned long)
			   ((unsigned long)lp +
			    (unsigned long)offset), 4));

	for (r = 0; r < count; r++) {
		dbg_pr("%p ", p);
		for (i = 0; i < 16; i++)
			dbg_pr("%02X", mdb_getword((unsigned long)
						   &p[i], 1));
		dbg_pr("  ");
		for (i = 0; i < 16; i++) {
			ch = mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		dbg_pr("\n");

		p = (void *)((unsigned long)p + 16);
	}

	return (unsigned char *)(mdb_getword((unsigned long)
					     ((unsigned long)lp +
					      (unsigned long)offset), 4));
}

unsigned char *dump_quad_stack(dbg_regs *dbgframe, unsigned char *p,
			       unsigned long count)
{
	register int i, r;
	u64 *lp;
	unsigned char ch;

	lp = (u64 *)p;

	dbg_pr("Stack = %04lX:%p\n",
	       (unsigned long)get_stack_segment(dbgframe), p);

	for (r = 0; r < count; r++) {
		dbg_pr("%04X:", get_stack_segment(dbgframe));
		dbg_pr("%p ", p);
		for (i = 0; i < (16 / 8); i++)
			dbg_pr(" %016llX",
			       (u64)mdb_getqword(&lp[i], 8));
		dbg_pr("  ");
		for (i = 0; i < 16; i++) {
			ch = mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		dbg_pr("\n");

		p = (void *)((unsigned long)p + 16);
		lp = (u64 *)p;
	}

	return p;
}

unsigned char *dump_double_stack(dbg_regs *dbgframe, unsigned char *p,
				 unsigned long count)
{
	register int i, r;
	u32 *lp;
	unsigned char ch;

	lp = (u32 *)p;

	dbg_pr("Stack = %04lX:%p\n",
	       (unsigned long)get_stack_segment(dbgframe), p);

	for (r = 0; r < count; r++) {
		dbg_pr("%04X:", get_stack_segment(dbgframe));
		dbg_pr("%p ", p);
		for (i = 0; i < (16 / 4); i++)
			dbg_pr(" %08X", mdb_getword((unsigned long)
						    &lp[i], 4));
		dbg_pr("  ");
		for (i = 0; i < 16; i++) {
			ch = mdb_getword((unsigned long)&p[i], 1);

			if (ch < 32 || ch > 126)
				dbg_pr(".");
			else
				dbg_pr("%c", ch);
		}
		dbg_pr("\n");

		p = (void *)((unsigned long)p + 16);
		lp = (u32 *)p;
	}

	return p;
}

#if IS_ENABLED(CONFIG_X86_64)
unsigned long dump_backtrace(unsigned char *p, unsigned long count)
{
	register int r;
	u64 *lp;

	lp = (u64 *)p;

	for (r = 0; r < count; r++) {
		if (__kernel_text_address(mdb_getqword((u64 *)lp, 8))) {
			if (dbg_pr("%p ", p))
				return 1;

			if (dbg_pr("%016llX ", mdb_getqword((u64 *)
							    lp, 8)))
				return 1;

			if (closest_symbol(mdb_getqword((u64 *)
							lp, 8)))
				if (dbg_pr("\n"))
					return 1;
		}
		p = (void *)((unsigned long)p + 8);
		lp = (u64 *)p;
	}

	return 0;
}

#else

unsigned long dump_backtrace(unsigned char *p, unsigned long count)
{
	register int r;
	unsigned long *lp;

	lp = (unsigned long *)p;

	for (r = 0; r < count; r++) {
		if (__kernel_text_address(mdb_getword((unsigned long)lp, 4))) {
			if (dbg_pr("%p ", p))
				return 1;

			if (dbg_pr("%08X ",
				   mdb_getword((unsigned long)lp,
					       4)))
				return 1;

			if (closest_symbol(mdb_getword((unsigned long)lp,
						       4)))
				if (dbg_pr("\n"))
					return 1;
		}
		p = (void *)((unsigned long)p + 4);
		lp = (unsigned long *)p;
	}

	return 0;
}
#endif

#if IS_ENABLED(CONFIG_X86_64)
unsigned char *dump_local_stack(dbg_regs *dbgframe, unsigned char *p,
				unsigned long count)
{
	register int r;
	u64 *lp;

	lp = (u64 *)p;

	dbg_pr("Stack = %04X:%p\n", get_stack_segment(dbgframe), p);

	for (r = 0; r < count; r++) {
		dbg_pr("%p ", p);
		dbg_pr("%016llX ", mdb_getqword((u64 *)lp, 8));
		if (closest_symbol(mdb_getqword((u64 *)lp, 8)))
			dbg_pr("\n");

		p = (void *)((unsigned long)p + 8);
		lp = (u64 *)p;
	}

	return p;
}

#else

unsigned char *dump_local_stack(dbg_regs *dbgframe, unsigned char *p,
				unsigned long count)
{
	register int r;
	unsigned long *lp;

	lp = (unsigned long *)p;

	dbg_pr("Stack = %04X:%p\n", get_stack_segment(dbgframe),
	       p);

	for (r = 0; r < count; r++) {
		dbg_pr("%p ", p);
		dbg_pr("%08X ", mdb_getword((unsigned long)lp, 4));
		if (closest_symbol(mdb_getword((unsigned long)lp, 4)))
			dbg_pr("\n");

		p = (void *)((unsigned long)p + 4);
		lp = (unsigned long *)p;
	}

	return p;
}
#endif

unsigned long display_toggle_help(unsigned char *command_line,
				  dbg_parser *parser)
{
	dbg_pr(".tb                      - userspace breakpoint (ON|OFF)\n");
	dbg_pr(".tc                      - control registers (ON|OFF)\n");
	dbg_pr(".tn                      - coprocessor register (ON|OFF)\n");
	dbg_pr(".ts                      - segment registers (ON|OFF)\n");
	dbg_pr(".tg                      - general registers (ON|OFF)\n");
	dbg_pr(".tr                      - display break reason (ON|OFF)\n");
	dbg_pr(".td                      - dereference display (ON|OFF)\n");
	dbg_pr(".tl                      - source line display (ON|OFF)\n");
	dbg_pr(".tu                      - unasm debug display (ON|OFF)\n");
	dbg_pr(".t or .t <address>       - show task state segment\n");
	dbg_pr(".toggle                  - show toggle settings\n");
	dbg_pr("toggle                   - show toggle settings\n");
	return 1;
}

unsigned long display_toggle_all(unsigned char *cmd,
				 dbg_regs *dbgframe, unsigned long exception,
				 dbg_parser *parser)
{
	dbg_pr("unasm debug display           : (%s)\n",
	       debug_deref ? "ON" : "OFF");
	dbg_pr("full dereferencing info       : (%s)\n",
	       full_deref_toggle ? "ON" : "OFF");
	dbg_pr("source line info              : (%s)\n",
	       line_info_toggle ? "ON" : "OFF");
	dbg_pr("general registers             : (%s)\n",
	       general_toggle ? "ON" : "OFF");
	dbg_pr("disable debugger in userspace : (%s)\n",
	       toggle_user_break ? "ON" : "OFF");
	dbg_pr("control registers             : (%s)\n",
	       control_toggle ? "ON" : "OFF");
	dbg_pr("user space memory read/write  : (%s)\n",
	       user_toggle ? "ON" : "OFF");
	dbg_pr("coprocessor registers         : (%s)\n",
	       numeric_toggle ? "ON" : "OFF");
	dbg_pr("display break reason          : (%s)\n",
	       reason_toggle ? "ON" : "OFF");
	dbg_pr("segment registers             : (%s)\n",
	       segment_toggle ? "ON" : "OFF");

	return 1;
}

/* .TU */

unsigned long process_tu_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(debug_deref)
		? (debug_deref = 0)
		: (debug_deref = 1);
	dbg_pr("unasm dereferencing (%s)\n", debug_deref ? "ON" : "OFF");
	return 1;
}

/* .TD */

unsigned long process_td_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(full_deref_toggle)
		? (full_deref_toggle = 0)
		: (full_deref_toggle = 1);
	dbg_pr("full dereferencing info (%s)\n",
	       full_deref_toggle ? "ON" : "OFF");
	return 1;
}

/* .TL */

unsigned long process_tl_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(line_info_toggle)
		? (line_info_toggle = 0)
		: (line_info_toggle = 1);
	dbg_pr("source line info (%s)\n", line_info_toggle ? "ON" : "OFF");
	return 1;
}

/* .TG */

unsigned long process_tg_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(general_toggle)
		? (general_toggle = 0)
		: (general_toggle = 1);
	dbg_pr("general registers (%s)\n", general_toggle ? "ON" : "OFF");
	return 1;
}

/* .TB */

unsigned long process_tb_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(toggle_user_break)
		? (toggle_user_break = 0)
		: (toggle_user_break = 1);
	dbg_pr("disable breakpoints in user address space (%s)\n",
	       control_toggle ? "ON" : "OFF");
	return 1;
}

/* .TC */

unsigned long process_tc_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(control_toggle)
		? (control_toggle = 0)
		: (control_toggle = 1);
	dbg_pr("control registers (%s)\n", control_toggle ? "ON" : "OFF");
	return 1;
}

/* .TM */

unsigned long process_toggle_user(unsigned char *cmd,
				  dbg_regs *dbgframe, unsigned long exception,
				  dbg_parser *parser)
{
	(user_toggle)
		? (user_toggle = 0)
		: (user_toggle = 1);
	dbg_pr("user space read/write for pages < PAGE_OFFSET (%s)\n",
	       user_toggle ? "ON" : "OFF");
	return 1;
}

/* .TN */

unsigned long process_tn_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(numeric_toggle)
		? (numeric_toggle = 0)
		: (numeric_toggle = 1);
	dbg_pr("coprocessor registers (%s)\n", numeric_toggle ? "ON" : "OFF");
	return 1;
}

/* .TR */

unsigned long process_tr_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(reason_toggle)
		? (reason_toggle = 0)
		: (reason_toggle = 1);
	dbg_pr("display break reason (%s)\n", reason_toggle ? "ON" : "OFF");
	return 1;
}

/* .TS */

unsigned long process_ts_toggle(unsigned char *cmd,
				dbg_regs *dbgframe, unsigned long exception,
				dbg_parser *parser)
{
	(segment_toggle)
		? (segment_toggle = 0)
		: (segment_toggle = 1);
	dbg_pr("segment registers (%s)\n", segment_toggle ? "ON" : "OFF");
	return 1;
}

unsigned long display_debugger_version_help(unsigned char *command_line,
					    dbg_parser *parser)
{
	dbg_pr(".v                       - display version info\n");
	return 1;
}

/* .V */

unsigned long display_debugger_version(unsigned char *cmd,
				       dbg_regs *dbgframe,
				       unsigned long exception,
				       dbg_parser *parser)
{
	dbg_pr("Minimal Kernel Debugger\n");
	dbg_pr("v%ld\n", 1);

	return 1;
}

/* .Z */

unsigned long display_symbols_help(unsigned char *command_line,
				   dbg_parser *parser)
{
	dbg_pr(".z <name|partial name>     - search symbols\n");
	dbg_pr("symbol <name|partial name> - search symbols\n");
	return 1;
}

unsigned long display_symbols(unsigned char *cmd,
			      dbg_regs *dbgframe, unsigned long exception,
			      dbg_parser *parser)
{
	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (*cmd)
		dump_os_symbol_table_match(cmd);
	else
		dump_os_symbol_table_match(NULL);

	return 1;
}

/* LCPU */

unsigned long list_processors(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	register int i;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	dbg_pr("Current Processor: %d\n", get_processor_id());
	dbg_pr("Active Processors:\n");
	for (i = 0; i < MAX_PROCESSORS; i++) {
		if (cpu_online(i))
			dbg_pr("   Processor %d\n", i);
	}
	return 1;
}

unsigned long clear_screen_help(unsigned char *command_line,
				dbg_parser *parser)
{
	dbg_pr("cls                      - clear the screen\n");
	return 1;
}

/* CLS */

unsigned long clear_debugger_screen(unsigned char *cmd,
				    dbg_regs *dbgframe,
				    unsigned long exception,
				    dbg_parser *parser)
{
	clear_screen();
	return 1;
}

unsigned long search_memory_help(unsigned char *command_line,
				 dbg_parser *parser)
{
	dbg_pr("s                        - search for bytes at address\n");
	dbg_pr("sb                       - search for bytes at address\n");
	dbg_pr("sw                       - search for words at address\n");
	dbg_pr("sd                       - search for dwords at address\n");
	dbg_pr("sq                       - search for qwords at address\n");
	return 1;
}

/* S */

/* use local storage and reduce stack space use.  these functions are always
 * called single threaded from the console
 */

unsigned char s_change_buffer[16];
unsigned char b_search_buffer[16];
unsigned char b_copy_buffer[16];
unsigned short w_search_buffer[16];
unsigned short w_copy_buffer[16];
u32 d_search_buffer[16];
u32 d_copy_buffer[16];
u64 q_search_buffer[16];
u64 q_copy_buffer[16];

unsigned long search_memory(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser)
{
	unsigned char *change_buffer = s_change_buffer;
	unsigned char *search_buffer = b_search_buffer;
	unsigned char *copy_buffer = b_copy_buffer;
	unsigned long maxlen = sizeof(b_search_buffer);
	register unsigned char *changeB;
	unsigned char *pB;
	register unsigned long address, r, value, count, len, i;
	unsigned long valid, ending_address = (unsigned long)high_memory;
	register int key;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	memset((unsigned long *)search_buffer, 0, sizeof(b_search_buffer));
	count = 0;
	changeB = (unsigned char *)search_buffer;
	change_buffer[0] = '\0';
	dbg_pr("enter bytes to search for, '.' to end input\n");
	while ((change_buffer[0] != '.') && (count < maxlen)) {
		for (r = 0; r < 8; r++) {
			dbg_pr("0x");

			in_keyboard(&change_buffer[0], 0, 4);

			if ((change_buffer[0] == '.') ||
			    (change_buffer[1] == '.'))
				break;

			pB = (unsigned char *)&change_buffer[0];
			len = strlen(pB);

			for (i = 0; i < len; i++)
				dbg_pr("\b");

			value = eval_expr(0, &pB, &valid);
			if (valid)
				*changeB = (unsigned char)value;
			dbg_pr("%02X ", (unsigned char)*changeB);

			changeB++;
			if (count++ > maxlen)
				break;
		}
		if (dbg_pr("\n"))
			return 1;
	}

	if (count) {
		dbg_pr("enter start address for search:  ");
		in_keyboard(&change_buffer[0], 0, 16);
		pB = (unsigned char *)&change_buffer[0];
		address = eval_expr(0, &pB, &valid);
		if (valid) {
			register unsigned long temp;

			dbg_pr("start address =[%p]\n",
			       (unsigned int *)address);
			dbg_pr("enter ending address for search:  ");

			in_keyboard(&change_buffer[0], 0, 16);
			pB = (unsigned char *)&change_buffer[0];
			temp = eval_expr(0, &pB, &valid);
			if (valid)
				ending_address = temp;

			dbg_pr("\nsearching memory from 0x%p to 0x%p\n",
			       (unsigned int *)address,
			       (unsigned int *)ending_address);
			while (address < ending_address) {
				read_memory((void *)address, copy_buffer,
					    count);
				if (!memcmp(search_buffer, copy_buffer,
					    count)) {
					if (dbg_pr("match at address[%p]\n",
						   (unsigned int *)address))
						return 1;
					if (search_results((unsigned char *)
							   address, 4))
						return 1;
					if (dbg_pr("searching\n"))
						return 1;
				}
				address++;
				if (!(address % 0x100000)) {
					if (dbg_pr("searching 0x%p...\n",
						   (unsigned int *)address))
						return 1;
					key = mdb_getkey();
					if (((char)key == 'Q') ||
					    ((char)key == 'q'))
						break;
				}
			}
			if (dbg_pr("search completed.\n"))
				return 1;
			return 1;
		}
		if (dbg_pr("invalid start address\n"))
			return 1;
		return 1;
	}
	if (dbg_pr("no search pattern\n"))
		return 1;
	return 1;
}

/* SB */

unsigned long search_memory_b(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	unsigned char *change_buffer = s_change_buffer;
	unsigned char *search_buffer = b_search_buffer;
	unsigned char *copy_buffer = b_copy_buffer;
	unsigned long maxlen = sizeof(b_search_buffer);
	register unsigned char *changeB;
	unsigned char *pB;
	register unsigned long address, r, value, count, len, i;
	unsigned long valid, ending_address = (unsigned long)high_memory;
	register int key;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	memset((unsigned long *)search_buffer, 0, sizeof(b_search_buffer));
	count = 0;
	changeB = (unsigned char *)search_buffer;
	change_buffer[0] = '\0';
	dbg_pr("enter bytes to search for, '.' to end input\n");
	while (change_buffer[0] != '.' && count < maxlen) {
		for (r = 0; r < 8; r++) {
			dbg_pr("0x");

			in_keyboard(&change_buffer[0], 0, 4);

			if ((change_buffer[0] == '.') ||
			    (change_buffer[1] == '.'))
				break;

			pB = (unsigned char *)&change_buffer[0];
			len = strlen(pB);
			for (i = 0; i < len; i++)
				dbg_pr("\b");

			value = eval_expr(0, &pB, &valid);
			if (valid)
				*changeB = (unsigned char)value;
			dbg_pr("%02X ", (unsigned char)*changeB);

			changeB++;
			if (count++ > maxlen)
				break;
		}
		if (dbg_pr("\n"))
			return 1;
	}

	if (count) {
		dbg_pr("enter start address for search:  ");
		in_keyboard(&change_buffer[0], 0, 16);
		pB = (unsigned char *)&change_buffer[0];
		address = eval_expr(0, &pB, &valid);
		if (valid) {
			register unsigned long temp;

			dbg_pr("start address =[%p]\n",
			       (unsigned int *)address);

			dbg_pr("enter ending address for search:  ");
			in_keyboard(&change_buffer[0], 0, 16);
			pB = (unsigned char *)&change_buffer[0];
			temp = eval_expr(0, &pB, &valid);
			if (valid)
				ending_address = temp;

			dbg_pr("\nsearching memory from 0x%p to 0x%p\n",
			       (unsigned int *)address,
			       (unsigned int *)ending_address);
			while (address < ending_address) {
				read_memory((void *)address, copy_buffer,
					    count);
				if (!memcmp(search_buffer, copy_buffer,
					    count)) {
					if (dbg_pr("match at address[%p]\n",
						   (unsigned int *)address))
						return 1;
					if (search_results((unsigned char *)
							   address, 4))
						return 1;
					if (dbg_pr("searching\n"))
						return 1;
				}
				address++;
				if (!(address % 0x100000)) {
					if (dbg_pr("searching 0x%p ...\n",
						   (unsigned int *)address))
						return 1;
					key = mdb_getkey();
					if (((char)key == 'Q') ||
					    ((char)key == 'q'))
						break;
				}
			}
			if (dbg_pr("search completed.\n"))
				return 1;
			return 1;
		}
		if (dbg_pr("invalid start address\n"))
			return 1;
		return 1;
	}
	if (dbg_pr("no search pattern\n"))
		return 1;
	return 1;
}

/* SW */

unsigned long search_memory_w(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	unsigned char *change_buffer = s_change_buffer;
	unsigned short *search_buffer = w_search_buffer;
	unsigned short *copy_buffer = w_copy_buffer;
	unsigned long maxlen = ARRAY_SIZE(w_search_buffer);
	register unsigned short *changeW;
	unsigned char *pB;
	register unsigned long address, r, value, count, len, i;
	unsigned long valid, ending_address = (unsigned long)high_memory;
	register int key;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	memset((unsigned long *)search_buffer, 0, sizeof(w_search_buffer));
	count = 0;
	changeW = (unsigned short *)search_buffer;
	change_buffer[0] = '\0';
	dbg_pr("enter words to search for, '.' to end input\n");
	while (change_buffer[0] != '.' && count < maxlen) {
		for (r = 0; r < 4; r++) {
			dbg_pr("0x");

			in_keyboard(&change_buffer[0], 0, 6);

			if ((change_buffer[0] == '.') ||
			    (change_buffer[1] == '.') ||
			    (change_buffer[2] == '.') ||
			    (change_buffer[3] == '.'))
				break;

			pB = (unsigned char *)&change_buffer[0];
			len = strlen(pB);
			for (i = 0; i < len; i++)
				dbg_pr("\b");

			value = eval_expr(0, &pB, &valid);
			if (valid)
				*changeW = value;
			dbg_pr("%04X ", *changeW);

			changeW++;
			if (count++ > maxlen)
				break;
		}
		if (dbg_pr("\n"))
			return 1;
	}

	if (count) {
		dbg_pr("enter start address for search:  ");
		in_keyboard(&change_buffer[0], 0, 16);
		pB = (unsigned char *)&change_buffer[0];
		address = eval_expr(0, &pB, &valid);
		if (valid) {
			register unsigned long temp;

			dbg_pr("start address =[%p]\n",
			       (unsigned int *)address);

			dbg_pr("enter ending address for search:  ");
			in_keyboard(&change_buffer[0], 0, 16);
			pB = (unsigned char *)&change_buffer[0];
			temp = eval_expr(0, &pB, &valid);
			if (valid)
				ending_address = temp;

			dbg_pr("searching memory from 0x%p to 0x%p\n",
			       (unsigned int *)address,
			       (unsigned int *)ending_address);
			while (address < ending_address) {
				read_memory((void *)address, copy_buffer,
					    count * sizeof(unsigned short));
				if (!memcmp(search_buffer, copy_buffer,
					    count * sizeof(unsigned short))) {
					if (dbg_pr("match at address[%p]\n",
						   (unsigned int *)address))
						return 1;
					if (dump_word_search((unsigned char *)
							     address, 4))
						return 1;
					if (dbg_pr("searching\n"))
						return 1;
				}
				address++;
				if (!(address % 0x100000)) {
					if (dbg_pr("searching 0x%p ...\n",
						   (unsigned int *)address))
						return 1;
					key = mdb_getkey();
					if (((char)key == 'Q') ||
					    ((char)key == 'q'))
						break;
				}
			}
			if (dbg_pr("search completed.\n"))
				return 1;
			return 1;
		}
		if (dbg_pr("invalid start address\n"))
			return 1;
		return 1;
	}
	if (dbg_pr("no search pattern\n"))
		return 1;
	return 1;
}

/* SD */

unsigned long search_memory_d(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned char *change_buffer = s_change_buffer;
	register u32 *search_buffer = d_search_buffer;
	register u32 *copy_buffer = d_copy_buffer;
	register unsigned long maxlen = ARRAY_SIZE(d_search_buffer);
	register unsigned long *changeD;
	unsigned char *pB;
	register unsigned long address, r, value, count, len, i;
	unsigned long valid, ending_address = (unsigned long)high_memory;
	register int key;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	memset((unsigned long *)search_buffer, 0, sizeof(d_search_buffer));
	count = 0;
	changeD = (unsigned long *)search_buffer;
	change_buffer[0] = '\0';
	dbg_pr("enter dwords to search for, '.' to end input\n");
	while (change_buffer[0] != '.' && count < maxlen) {
		for (r = 0; r < 2; r++) {
			dbg_pr("0x");

			in_keyboard(&change_buffer[0], 0, 8);

			if ((change_buffer[0] == '.') ||
			    (change_buffer[1] == '.') ||
			    (change_buffer[2] == '.') ||
			    (change_buffer[3] == '.') ||
			    (change_buffer[4] == '.') ||
			    (change_buffer[5] == '.') ||
			    (change_buffer[6] == '.') ||
			    (change_buffer[7] == '.'))
				break;

			pB = (unsigned char *)&change_buffer[0];
			len = strlen(pB);
			for (i = 0; i < len; i++)
				dbg_pr("\b");

			value = eval_expr(0, &pB, &valid);
			if (valid)
				*changeD = value;
			dbg_pr("%08X ", *changeD);

			changeD++;
			if (count++ > maxlen)
				break;
		}
		if (dbg_pr("\n"))
			return 1;
	}

	if (count) {
		dbg_pr("enter start address for search:  ");
		in_keyboard(&change_buffer[0], 0, 16);
		pB = (unsigned char *)&change_buffer[0];
		address = eval_expr(0, &pB, &valid);
		if (valid) {
			register unsigned long temp;

			dbg_pr("start address =[%p]\n",
			       (unsigned int *)address);

			dbg_pr("enter ending address for search:  ");
			in_keyboard(&change_buffer[0], 0, 16);
			pB = (unsigned char *)&change_buffer[0];
			temp = eval_expr(0, &pB, &valid);
			if (valid)
				ending_address = temp;

			dbg_pr("searching memory from 0x%p to 0x%p\n",
			       (unsigned int *)address,
			       (unsigned int *)ending_address);
			while (address < ending_address) {
				read_memory((void *)address, copy_buffer,
					    count * sizeof(unsigned long));
				if (!memcmp(search_buffer, copy_buffer,
					    count * sizeof(unsigned long))) {
					if (dbg_pr("match at address[%p]\n",
						   (unsigned int *)address))
						return 1;
					if (dump_double_search((unsigned char *)
							       address, 4))
						return 1;
					if (dbg_pr("searching\n"))
						return 1;
				}
				address++;
				if (!(address % 0x100000)) {
					if (dbg_pr("searching 0x%p ...\n",
						   (unsigned int *)address))
						return 1;
					key = mdb_getkey();
					if (((char)key == 'Q') ||
					    ((char)key == 'q'))
						break;
				}
			}
			if (dbg_pr("search completed.\n"))
				return 1;
			return 1;
		}
		if (dbg_pr("invalid start address\n"))
			return 1;
		return 1;
	}
	if (dbg_pr("no search pattern\n"))
		return 1;
	return 1;
}

/* SQ */

unsigned long search_memory_q(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser)
{
	register unsigned char *change_buffer = s_change_buffer;
	register u64 *search_buffer = q_search_buffer;
	register u64 *copy_buffer = q_copy_buffer;
	register unsigned long maxlen = ARRAY_SIZE(q_search_buffer);
	register unsigned long *changeQ;
	unsigned char *pB;
	register unsigned long address, r, value, count, len, i;
	unsigned long valid, ending_address = (unsigned long)high_memory;
	register int key;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	memset((unsigned long *)search_buffer, 0, sizeof(d_search_buffer));
	count = 0;
	changeQ = (unsigned long *)search_buffer;
	change_buffer[0] = '\0';
	dbg_pr("enter dwords to search for, '.' to end input\n");
	while (change_buffer[0] != '.' && count < maxlen) {
		for (r = 0; r < 1; r++) {
			dbg_pr("0x");

			in_keyboard(&change_buffer[0], 0, 8);

			if ((change_buffer[0] == '.') ||
			    (change_buffer[1] == '.') ||
			    (change_buffer[2] == '.') ||
			    (change_buffer[3] == '.') ||
			    (change_buffer[4] == '.') ||
			    (change_buffer[5] == '.') ||
			    (change_buffer[6] == '.') ||
			    (change_buffer[7] == '.') ||
			    (change_buffer[8] == '.') ||
			    (change_buffer[9] == '.') ||
			    (change_buffer[10] == '.') ||
			    (change_buffer[11] == '.') ||
			    (change_buffer[12] == '.') ||
			    (change_buffer[13] == '.') ||
			    (change_buffer[14] == '.') ||
			    (change_buffer[15] == '.'))
				break;

			pB = (unsigned char *)&change_buffer[0];
			len = strlen(pB);
			for (i = 0; i < len; i++)
				dbg_pr("\b");

			value = eval_expr(0, &pB, &valid);
			if (valid)
				*changeQ = value;
			dbg_pr("%016X ", *changeQ);

			changeQ++;
			if (count++ > maxlen)
				break;
		}
		if (dbg_pr("\n"))
			return 1;
	}

	if (count) {
		dbg_pr("enter start address for search:  ");
		in_keyboard(&change_buffer[0], 0, 16);
		pB = (unsigned char *)&change_buffer[0];
		address = eval_expr(0, &pB, &valid);
		if (valid) {
			register unsigned long temp;

			dbg_pr("start address =[%p]\n",
			       (unsigned int *)address);

			dbg_pr("enter ending address for search:  ");
			in_keyboard(&change_buffer[0], 0, 16);
			pB = (unsigned char *)&change_buffer[0];
			temp = eval_expr(0, &pB, &valid);
			if (valid)
				ending_address = temp;

			dbg_pr("searching memory from 0x%p to 0x%p\n",
			       (unsigned int *)address,
			       (unsigned int *)ending_address);
			while (address < ending_address) {
				read_memory((void *)address, copy_buffer,
					    count * sizeof(unsigned long long));
				if (!memcmp(search_buffer, copy_buffer,
					    count *
					    sizeof(unsigned long long))) {
					if (dbg_pr("match at address[%p]\n",
						   (unsigned int *)address))
						return 1;
					if (dump_quad((unsigned char *)address,
						      4, 0))
						return 1;
					if (dbg_pr("searching\n"))
						return 1;
				}
				address++;
				if (!(address % 0x100000)) {
					if (dbg_pr("searching 0x%p ...\n",
						   (unsigned int *)address))
						return 1;
					key = mdb_getkey();
					if (((char)key == 'Q') ||
					    ((char)key == 'q'))
						break;
				}
			}
			if (dbg_pr("search completed.\n"))
				return 1;
			return 1;
		}
		if (dbg_pr("invalid start address\n"))
			return 1;
		return 1;
	}
	if (dbg_pr("no search pattern\n"))
		return 1;
	return 1;
}

unsigned long change_memory_help(unsigned char *command_line,
				 dbg_parser *parser)
{
	dbg_pr("c   <address>            - change bytes at address\n");
	dbg_pr("cb  <address>            - change bytes at address\n");
	dbg_pr("cw  <address>            - change words at address\n");
	dbg_pr("cd  <address>            - change dwords at address\n");
	dbg_pr("cq  <address>            - change qwords at address\n");
	return 1;
}

/* CW */

unsigned long change_word_value(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	register unsigned char *change_buffer = &workbuf[0];
	register unsigned short *changeW, old_w;
	unsigned char *pB;
	register unsigned long address, r, value, len, i;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		changeW = (unsigned short *)address;
		change_buffer[0] = '\0';
		dbg_pr("enter new value, <enter> to skip, or '.' to exit\n");
		while (change_buffer[0] != '.') {
			dbg_pr("[%p] ", changeW);
			for (r = 0; r < 4; r++) {
				old_w = (unsigned short)
					mdb_getword((unsigned long)changeW, 2);
				dbg_pr("(%04X)=", old_w);

				in_keyboard(&change_buffer[0], 0, 6);

				if ((change_buffer[0] == '.') ||
				    (change_buffer[1] == '.') ||
				    (change_buffer[2] == '.') ||
				    (change_buffer[3] == '.'))
					break;
				pB = (unsigned char *)&change_buffer[0];
				len = strlen(pB);

				for (i = 0; i < len; i++)
					dbg_pr("\b");

				value = eval_expr(0, &pB, &valid);
				if (valid)
					mdb_putword((unsigned long)changeW,
						    value, 2);
				dbg_pr("%04X ",
				       mdb_getword((unsigned long)changeW,
						   2));
				changeW++;
			}
			if (dbg_pr("\n"))
				return 1;
		}
		return 1;
	}
	dbg_pr("invalid change (word) address\n");
	return 1;
}

/* CQ */

unsigned long change_quad_value(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	register unsigned char *change_buffer = &workbuf[0];
	register u64 *changeD, old_d;
	register unsigned long address, r, value, len, i;
	unsigned char *pB;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		changeD = (u64 *)address;
		change_buffer[0] = '\0';
		dbg_pr("enter new value, <enter> to skip, or '.' to exit\n");
		while (change_buffer[0] != '.') {
			dbg_pr("[%p] ", changeD);
			for (r = 0; r < 2; r++) {
				old_d = mdb_getqword(changeD, 8);
				dbg_pr("(%016llX)=", old_d);

				in_keyboard(&change_buffer[0], 0,
					    16);

				if ((change_buffer[0] == '.') ||
				    (change_buffer[1] == '.') ||
				    (change_buffer[2] == '.') ||
				    (change_buffer[3] == '.') ||
				    (change_buffer[4] == '.') ||
				    (change_buffer[5] == '.') ||
				    (change_buffer[6] == '.') ||
				    (change_buffer[7] == '.') ||
				    (change_buffer[8] == '.') ||
				    (change_buffer[9] == '.') ||
				    (change_buffer[10] == '.') ||
				    (change_buffer[11] == '.') ||
				    (change_buffer[12] == '.') ||
				    (change_buffer[13] == '.') ||
				    (change_buffer[14] == '.') ||
				    (change_buffer[15] == '.'))
					break;

				pB = (unsigned char *)&change_buffer[0];
				len = strlen(pB);

				for (i = 0; i < len; i++)
					dbg_pr("\b");

				value = eval_expr(0, &pB, &valid);
				if (valid)
					mdb_putqword(changeD, value, 8);
				dbg_pr("%016llX ", mdb_getqword(changeD, 8));
				changeD++;
			}
			if (dbg_pr("\n"))
				return 1;
		}
		return 1;
	}
	dbg_pr("invalid change (qword) address\n");
	return 1;
}

/* CD */

unsigned long change_double_value(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	register unsigned char *change_buffer = &workbuf[0];
	register u32 *changeD, old_d;
	register unsigned long address, r, value, len, i;
	unsigned char *pB;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		changeD = (u32 *)address;
		change_buffer[0] = '\0';
		dbg_pr("enter new value, <enter> to skip, or '.' to exit\n");
		while (change_buffer[0] != '.') {
			dbg_pr("[%p] ", changeD);
			for (r = 0; r < 2; r++) {
				old_d = (unsigned long)
					mdb_getword((unsigned long)changeD, 4);
				dbg_pr("(%08X)=", old_d);

				in_keyboard(&change_buffer[0], 0, 8);

				if ((change_buffer[0] == '.') ||
				    (change_buffer[1] == '.') ||
				    (change_buffer[2] == '.') ||
				    (change_buffer[3] == '.') ||
				    (change_buffer[4] == '.') ||
				    (change_buffer[5] == '.') ||
				    (change_buffer[6] == '.') ||
				    (change_buffer[7] == '.'))
					break;

				pB = (unsigned char *)&change_buffer[0];
				len = strlen(pB);

				for (i = 0; i < len; i++)
					dbg_pr("\b");

				value = eval_expr(0, &pB, &valid);
				if (valid)
					mdb_putword((unsigned long)changeD,
						    value, 4);
				dbg_pr("%08X ",
				       mdb_getword((unsigned long)changeD,
						   4));
				changeD++;
			}
			if (dbg_pr("\n"))
				return 1;
		}
		return 1;
	}
	dbg_pr("invalid change (dword) address\n");
	return 1;
}

/* CB */

unsigned long change_byte_value(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	unsigned char *change_buffer = &workbuf[0];
	register unsigned char *changeB, oldB;
	unsigned char *pB;
	register unsigned long address, r, value, len, i;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		changeB = (unsigned char *)address;
		change_buffer[0] = '\0';
		dbg_pr("enter new value, <enter> to skip, or '.' to exit\n");
		while (change_buffer[0] != '.') {
			dbg_pr("[%p] ", changeB);
			for (r = 0; r < 8; r++) {
				oldB = (unsigned char)
					mdb_getword((unsigned long)changeB, 1);
				dbg_pr("(%02X)=", oldB);

				in_keyboard(&change_buffer[0], 0, 4);

				if ((change_buffer[0] == '.') ||
				    (change_buffer[1] == '.'))
					break;

				pB = (unsigned char *)&change_buffer[0];
				len = strlen(pB);
				for (i = 0; i < len; i++)
					dbg_pr("\b");

				value = eval_expr(0, &pB, &valid);
				if (valid)
					mdb_putword((unsigned long)changeB,
						    value, 1);
				dbg_pr("%02X ", (unsigned char)
				       mdb_getword((unsigned long)changeB,
						   1));
				changeB++;
			}
			if (dbg_pr("\n"))
				return 1;
		}
		return 1;
	}
	dbg_pr("invalid change (byte) address\n");
	return 1;
}

/* C */

unsigned long change_default_value(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser)
{
	unsigned char *change_buffer = &workbuf[0];
	register unsigned char *changeB, oldB;
	unsigned char *pB;
	register unsigned long address, r, value, len, i;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	address = eval_expr(dbgframe, &cmd, &valid);
	if (valid) {
		changeB = (unsigned char *)address;
		change_buffer[0] = '\0';
		dbg_pr("enter new value, <enter> to skip, or '.' to exit\n");
		while (change_buffer[0] != '.') {
			dbg_pr("[%p] ", changeB);
			for (r = 0; r < 8; r++) {
				oldB = (unsigned char)
					mdb_getword((unsigned long)changeB, 1);
				dbg_pr("(%02X)=", (unsigned char)oldB);

				in_keyboard(&change_buffer[0], 0, 4);

				if ((change_buffer[0] == '.') ||
				    (change_buffer[1] == '.'))
					break;

				pB = (unsigned char *)&change_buffer[0];
				len = strlen(pB);

				for (i = 0; i < len; i++)
					dbg_pr("\b");

				value = eval_expr(0, &pB, &valid);
				if (valid)
					mdb_putword((unsigned long)changeB,
						    value, 1);
				dbg_pr("%02X ", (unsigned char)
				       mdb_getword((unsigned long)changeB,
						   1));
				changeB++;
			}
			if (dbg_pr("\n"))
				return 1;
		}
		return 1;
	}
	dbg_pr("invalid change (byte) address\n");
	return 1;
}

unsigned long display_close_help(unsigned char *command_line,
				 dbg_parser *parser)
{
	dbg_pr("? <address>              - closest symbols to address\n");
	return 1;
}

/* ? */

unsigned long display_close_symbols(unsigned char *cmd,
				    dbg_regs *dbgframe,
				    unsigned long exception,
				    dbg_parser *parser)
{
	register unsigned long old_d;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	old_d = eval_expr(dbgframe, &cmd, &valid);
	if (!valid)
		old_d = get_ip(dbgframe);
	closest_symbol(old_d);
	return 1;
}

unsigned long debugger_walk_stack(unsigned char *cmd,
				  dbg_regs *dbgframe, unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_local_stack(dbgframe,
					     (unsigned char *)last_dump,
					     last_display);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_local_stack(dbgframe,
				     (unsigned char *)last_dump,
				     display_length);
	return 1;
}

unsigned long display_dump_help(unsigned char *command_line,
				dbg_parser *parser)
{
	dbg_pr("d   <address> <#lines>   - dump memory as bytes (8 bit)\n");
	dbg_pr("dw  <address> <#lines>   - dump memory as words (16 bit)\n");
	dbg_pr("dd  <address> <#lines>   - dump memory as dwords (32 bit)\n");
	dbg_pr("dq  <address> <#lines>   - dump memory as qwords (64 bit)\n");
	dbg_pr("dp  <address> <#lines>   - dump physical addr as bytes\n");
	dbg_pr("dpw <address> <#lines>   - dump physical addr as words\n");
	dbg_pr("dpd <address> <#lines>   - dump physical addr as dwords\n");
	dbg_pr("dpq <address> <#lines>   - dump physical addr as qwords\n");
	dbg_pr("ds  <address> <#lines>   - dump stack\n");
	dbg_pr("dds <address> <#lines>   - dump stack dword\n");
	dbg_pr("dqs <address> <#lines>   - dump stack qword\n");
	dbg_pr("w   <address>            - display symbols on the stack\n");

	return 1;
}

/* DL */

unsigned long debugger_dump_linked_list(unsigned char *cmd,
					dbg_regs *dbgframe,
					unsigned long exception,
					dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_link = dump_linked_list((unsigned char *)
					     last_link,
					     last_display, 0);
		return 1;
	}

#if IS_ENABLED(CONFIG_X86_64)
	last_link = (unsigned char *)eval_numeric_expr(dbgframe,
						       &cmd,
						       &valid);
#else
	address = eval_numeric_expr(dbgframe, &cmd, &valid);
	last_link = (unsigned char *)address;
#endif
	if (!valid)
		last_link = (unsigned char *)get_stack_address(dbgframe);

	display_length = eval_numeric_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;

	last_link = dump_linked_list((unsigned char *)last_link,
				     display_length, 0);

	return 1;
}

/* DS */

unsigned long debugger_dump_stack(unsigned char *cmd,
				  dbg_regs *dbgframe, unsigned long exception,
				  dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_local_stack(dbgframe, (unsigned char *)
					     last_dump, last_display);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_local_stack(dbgframe, (unsigned char *)
				     last_dump, display_length);
	return 1;
}

/* DDS */

unsigned long debugger_dump_double_stack(unsigned char *cmd,
					 dbg_regs *dbgframe,
					 unsigned long exception,
					 dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_double_stack(dbgframe,
					      (unsigned char *)
					      last_dump,
					      last_display);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_double_stack(dbgframe, (unsigned char *)
				      last_dump,
				      display_length);
	return 1;
}

/* DQS */

unsigned long debugger_dump_quad_stack(unsigned char *cmd,
				       dbg_regs *dbgframe,
				       unsigned long exception,
				       dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_quad_stack(dbgframe,
					    (unsigned char *)
					    last_dump,
					    last_display);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_quad_stack(dbgframe,
				    (unsigned char *)last_dump,
				    display_length);
	return 1;
}

/* DQ */

unsigned long debugger_dump_quad(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_quad((unsigned char *)last_dump,
				      last_display, 0);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_quad((unsigned char *)last_dump,
			      display_length, 0);
	return 1;
}

/* DD */

unsigned long debugger_dump_double(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_double((unsigned char *)last_dump,
					last_display, 0);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_double((unsigned char *)last_dump,
				display_length, 0);
	return 1;
}

/* DW */

unsigned long debugger_dump_word(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_word((unsigned char *)last_dump,
				      last_display, 0);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_word((unsigned char *)last_dump,
			      display_length, 0);
	return 1;
}

/* D */

unsigned long debugger_dump_byte(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump((unsigned char *)last_dump,
				 last_display, 0);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump((unsigned char *)last_dump,
			 display_length, 0);
	return 1;
}

/* DPQ */

unsigned long debugger_dump_quad_phys(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_quad((unsigned char *)last_dump,
				      last_display, 1);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_quad((unsigned char *)last_dump,
			      display_length, 1);
	return 1;
}

/* DPD */

unsigned long debugger_dump_double_phys(unsigned char *cmd,
					dbg_regs *dbgframe,
					unsigned long exception,
					dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_double((unsigned char *)last_dump,
					last_display, 1);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_double((unsigned char *)last_dump,
				display_length, 1);
	return 1;
}

/* DPW */

unsigned long debugger_dump_word_phys(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump_word((unsigned char *)last_dump,
				      last_display, 1);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump_word((unsigned char *)last_dump,
			      display_length, 1);
	return 1;
}

/* DP */

unsigned long debugger_dump_byte_phys(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser)
{
	unsigned long valid;
#if !IS_ENABLED(CONFIG_X86_64)
	unsigned long address;
#endif

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_dump = dump((unsigned char *)last_dump,
				 last_display, 1);
		return 1;
	}
#if IS_ENABLED(CONFIG_X86_64)
	last_dump = (unsigned char *)eval_expr(dbgframe, &cmd,
					       &valid);
#else
	address = eval_expr(dbgframe, &cmd, &valid);
	last_dump = (unsigned char *)address;
#endif
	if (!valid)
		last_dump = (unsigned char *)get_stack_address(dbgframe);
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_dump = dump((unsigned char *)last_dump, display_length,
			 1);
	return 1;
}

unsigned long display_disassemble_help(unsigned char *command_line,
				       dbg_parser *parser)
{
	dbg_pr("id  <address> <#lines>   - unassemble code (GNU format)\n");
	dbg_pr("u   <address> <#lines>   - unassemble code (INTEL format)\n");
	dbg_pr("ux  <address> <#lines>   - unassemble code (32-bit)\n");
	dbg_pr("uu  <address> <#lines>   - unassemble code (16-bit)\n");
	return 1;
}

/* UU */

unsigned long process_disasm_16(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_unasm = disassemble(dbgframe,
					 (unsigned long)last_unasm,
					 last_display, 0, 0);
		return 1;
	}
	last_unasm = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		if (!*cmd) {
			last_unasm = get_ip(dbgframe);
		} else {
			dbg_pr("invalid address for unassemble\n");
			return 1;
		}
	}
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_unasm = disassemble(dbgframe,
				 (unsigned long)last_unasm,
				 display_length, 0, 0);
	return 1;
}

/* UX */

unsigned long process_disasm_32(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_unasm = disassemble(dbgframe,
					 (unsigned long)
					 last_unasm,
					 last_display, 1, 0);
		return 1;
	}

	last_unasm = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		if (!*cmd) {
			last_unasm = get_ip(dbgframe);
		} else {
			dbg_pr("invalid address for unassemble\n");
			return 1;
		}
	}
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_unasm = disassemble(dbgframe,
				 (unsigned long)last_unasm,
				 display_length, 1, 0);
	return 1;
}

/* U */

unsigned long process_disassemble_any(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser)
{
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_unasm = disassemble(dbgframe,
					 (unsigned long)
					 last_unasm,
					 last_display, -1, 0);
		return 1;
	}

	last_unasm = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		if (!*cmd) {
			last_unasm = get_ip(dbgframe);
		} else {
			dbg_pr("invalid address for unassemble\n");
			return 1;
		}
	}
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_unasm = disassemble(dbgframe,
				 (unsigned long)last_unasm,
				 display_length, -1, 0);
	return 1;
}

/* ID */

unsigned long process_disassemble_att(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser)
{
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	if (repeat_command) {
		last_unasm = disassemble(dbgframe,
					 (unsigned long)
					 last_unasm,
					 last_display, -1, 1);
		return 1;
	}

	last_unasm = eval_expr(dbgframe, &cmd, &valid);
	if (!valid) {
		if (!*cmd) {
			last_unasm = get_ip(dbgframe);
		} else {
			dbg_pr("invalid address for unassemble\n");
			return 1;
		}
	}
	display_length = eval_expr(dbgframe, &cmd, &valid);
	if (!display_length || display_length > 20)
		display_length = 20;
	last_unasm = disassemble(dbgframe,
				 (unsigned long)last_unasm,
				 display_length, -1, 1);
	return 1;
}
