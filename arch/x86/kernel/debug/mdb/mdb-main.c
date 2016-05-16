
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
#include <linux/io.h>
#include <linux/kdebug.h>
#include <linux/notifier.h>
#include <linux/sysrq.h>
#include <linux/input.h>
#include <linux/uaccess.h>
#include <linux/nmi.h>
#include <linux/clocksource.h>
#include <linux/nmi.h>

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

static unsigned long hptr;
static unsigned char hbuf[16][256];
/* remember non-repeating commands */
unsigned char delim_table[256];
static unsigned char work_buffer[256];
unsigned char verb_buffer[100];

atomic_t inmdb = { 0 };
unsigned char *mdb_oops;
unsigned char *last_mdb_oops;

static inline void set_delimiter(unsigned char c)
{
	delim_table[c & 0x0FF] = 1;
}

static void save_last_cmd(unsigned long processor)
{
	register int i;

	repeat_command = 0;
	last_cmd = toupper(debug_command[0]);
	last_display = display_length;
	atomic_set(&focus_active, 0);
	atomic_set(&per_cpu(trace_processors, processor), 0);

	for (i = 0; (i < 80) && (debug_command[i]); i++) {
		if ((debug_command[i] == '\n') || (debug_command[i] == '\r'))
			last_debug_command[i] = '\0';
		else
			last_debug_command[i] = debug_command[i];
	}
	last_debug_command[i] = '\0';
}

static atomic_t inmdb_processor[NR_CPUS];

void mdb_watchdogs(void)
{
	touch_softlockup_watchdog_sync();
	clocksource_touch_watchdog();

#if IS_ENABLED(CONFIG_TREE_RCU)
	rcu_cpu_stall_reset();
#endif

	touch_nmi_watchdog();
#if IS_ENABLED(CONFIG_HARDLOCKUP_DETECTOR)
	touch_hardlockup_watchdog();
#endif
}

int mdb(unsigned long reason, unsigned long error, void *frame)
{
	register unsigned long ret_code = 0, processor = get_processor_id();

	unsigned long flagstate;
	dbg_regs dbgframe;

	ret_code = altdbg_routine(reason, error, frame);
	if (ret_code)
		return ret_code;

	last_mdb_oops = NULL;
	if (mdb_oops) {
		last_mdb_oops = mdb_oops;
		mdb_oops = NULL;
	}

	/* if we are re-entered, report it */
	if (atomic_read(&inmdb_processor[processor])) {
		dbg_pr("MDB re-entered.  excp: %d err: %d processor: %d\n",
		       reason, error, processor);
	}

	local_irq_save(flagstate);
	atomic_inc(&inmdb);
	atomic_inc(&inmdb_processor[processor]);
	memset(&dbgframe, 0, sizeof(dbg_regs));
	/* snapshot last reported processor frame */
	read_dbg_regs(frame, &per_cpu(current_dbg_regs, processor),
		      processor);
	read_dbg_regs(frame, &dbgframe, processor);
	ret_code = debugger_entry(reason, &dbgframe, processor);
	write_dbg_regs(frame, &dbgframe, processor);
	atomic_dec(&inmdb_processor[processor]);
	atomic_dec(&inmdb);
	local_irq_restore(flagstate);

	return ret_code;
}

static inline void out_chars(int c, unsigned char ch)
{
	register int i;

	for (i = 0; i < c; i++)
		dbg_pr("%c", ch);
}

static inline void out_copy_chars_limit(int c, unsigned char ch,
					unsigned char *s, int limit)
{
	register int i;

	for (i = 0; (i < c) && (i < limit); i++)
		s[i] = ch;
	s[i] = '\0';
	dbg_pr("%s", s);
}

static inline void out_buffer_chars_limit(unsigned char *s, int limit)
{
	register int i;

	for (i = 0; (i < strlen(s)) && (i < limit); i++)
		dbg_pr("%c", s[i]);
}

static inline void out_buffer_chars_limit_index(unsigned char *s, int limit,
						int index)
{
	register int i;

	for (i = 0; (i < limit) && (s[i]); i++)
		dbg_pr("%c", s[index + i]);
}

static inline void out_string(unsigned char *s)
{
	dbg_pr("%s", s);
}

static inline void out_char(unsigned char ch)
{
	dbg_pr("%c", ch);
}

unsigned long in_keyboard(unsigned char *buf,
			  unsigned long buf_index,
			  unsigned long max_index)
{
	register unsigned long key;
	register unsigned char *p;
	register int i, r, temp;
	register unsigned long orig_index, hndx;

	if (buf_index > max_index)
		return 0;

	if (!max_index)
		return 0;

	orig_index = buf_index;

	p = (unsigned char *)((unsigned long)buf + (unsigned long)buf_index);
	for (i = 0; i < (max_index - buf_index); i++)
		*p++ = '\0';

	hndx = hptr;
	while (1) {
		key = mdb_getkey();

		if ((is_accelerator(key)) && (key != 13))
			return key;

		switch (key) {
		case -1:
			return key;

		case 8: /* backspace */
			if (buf_index) {
				register int delta;

				buf_index--;
				out_string("\b \b");

				delta = strlen(buf) - buf_index;
				out_chars(delta, ' ');
				out_chars(delta, '\b');

				p = (unsigned char *)&buf[buf_index];
				temp = buf_index;
				p++;
				while ((*p) && (temp < max_index))
					buf[temp++] = *p++;
				buf[temp] = '\0';

				delta = strlen(buf) - buf_index;
				out_buffer_chars_limit_index(buf, delta,
							     buf_index);
				out_chars(delta, '\b');
			}
			break;

		case K_P7: /* home */
			{
				unsigned char *s = &work_buffer[0];

				out_copy_chars_limit(buf_index, '\b', s, 255);
				buf_index = orig_index;
			}
			break;

		case K_P1: /* end */
			{
				unsigned char *s = &work_buffer[0];

				out_copy_chars_limit(buf_index, '\b', s, 255);
				out_buffer_chars_limit(buf, 255);
				buf_index = strlen(buf);
			}
			break;

		case K_P4: /* left arrow */
			if (buf_index) {
				buf_index--;
				out_string("\b");
			}
			break;

		case K_P6: /* right arrow */
			if (buf_index < strlen(buf)) {
				out_char(buf[buf_index]);
				buf_index++;
			}
			break;

		case K_PDOT:
			{
				register int delta;

				delta = strlen(buf) - buf_index;

				out_chars(delta, ' ');
				out_chars(delta, '\b');

				p = (unsigned char *)&buf[buf_index];
				temp = buf_index;
				p++;
				while ((*p) && (temp < max_index))
					buf[temp++] = *p++;
				buf[temp] = '\0';

				delta = strlen(buf) - buf_index;
				out_buffer_chars_limit_index(buf, delta,
							     buf_index);
				out_chars(delta, '\b');
			}
			break;

		case 13:  /* enter */
			if (strncmp(hbuf[(hptr - 1) & 0x0F], buf,
				    strlen(buf)) ||
			    (strlen(buf) != strlen(hbuf[(hptr - 1) & 0x0F]))) {
				for (r = 0; r < max_index; r++) {
					if (buf[0])
						hbuf[hptr & 0x0F][r] = buf[r];
				}
				if (buf[0])
					hptr++;
			}
			return 13;

		case K_P8: /* up arrow */
			if (hbuf[(hndx - 1) & 0x0F][0]) {
				unsigned char *s = &work_buffer[0];

				out_copy_chars_limit(buf_index, '\b', s, 255);
				out_copy_chars_limit(buf_index, ' ', s, 255);
				out_copy_chars_limit(buf_index, '\b', s, 255);

				hndx--;

				for (r = 0; r < max_index; r++)
					buf[r] = hbuf[hndx & 0x0F][r];
				buf_index = strlen(buf);

				out_string(buf);
			}
			break;

		case K_P2: /* down arrow */
			if (hbuf[hndx & 0x0F][0]) {
				unsigned char *s = &work_buffer[0];

				out_copy_chars_limit(buf_index, '\b', s, 255);
				out_copy_chars_limit(buf_index, ' ', s, 255);
				out_copy_chars_limit(buf_index, '\b', s, 255);

				hndx++;

				for (r = 0; r < max_index; r++)
					buf[r] = hbuf[hndx & 0x0F][r];
				buf_index = strlen(buf);

				out_string(buf);
			}
			break;

		default:
			/* if above or below text */
			if ((key > 0x7E) || (key < ' '))
				break;
			if (strlen(buf) < max_index) {
				register int delta;

				for (i = max_index; i > buf_index; i--)
					buf[i] = buf[i - 1];
				buf[buf_index] = (unsigned char)key;
				if (buf_index < max_index)
					buf_index++;

				delta = strlen(buf) - buf_index;
				out_buffer_chars_limit_index(buf, delta,
							     buf_index);
				out_chars(delta, '\b');
			}
			break;
		}
	}
}

unsigned long debugger_command_entry(unsigned long processor,
				     unsigned long exception,
				     dbg_regs *dbgframe)
{
	register unsigned char *verb, *pp, *vp;
	register unsigned long count, ret_code, key;

	if (exception > 22)
		exception = 20;

	last_unasm = (unsigned long)get_ip(dbgframe);
	last_dump =
		(unsigned char *)get_stack_address(dbgframe);
	last_link =
		(unsigned char *)get_stack_address(dbgframe);
	display_length = 20;
	last_display = 20;
	last_cmd_key = last_cmd;
	nextline = 0;
	pause_mode = 0;

	if (!ssbmode) {
		if (reason_toggle &&
		    !console_display_reason(dbgframe, exception,
						  processor, last_cmd))
			return 0;
		display_registers(dbgframe, processor);
	}
	next_unasm = disassemble(dbgframe, last_unasm, 1, -1, 0);
	clear_temp_breakpoints();
	if (ssb_update(dbgframe, processor) == -1)
		return 0;

	while (1) {
		pause_mode = 1;
		nextline = 0;
		dbg_pr("(%i)> ", (int)processor);

		save_last_cmd(processor);
		key = in_keyboard((unsigned char *)&debug_command[0], 0, 80);
		if (key == -1)
			return -1;

		if (key) {
			ret_code = accel_routine(key, dbgframe);
			switch (ret_code) {
			case 0:
				break;

			case -1:
				return ret_code;

			default:
				dbg_pr("\n");
				continue;
			}
		}

		if (*debug_command) {
			count = 0;
			pp = (unsigned char *)debug_command;
			vp = &verb_buffer[0];
			verb = vp;
			while (*pp && *pp == ' ' && count++ < 80)
				pp++;

			while (*pp && *pp != ' ' && *pp != '=' && count++ < 80)
				*vp++ = *pp++;
			*vp = '\0';

			while (*pp && *pp == ' ' && count++ < 80)
				pp++;

			ret_code =
				debugger_parser_routine(verb,
							(unsigned char *)
							debug_command,
							dbgframe, exception);
			switch (ret_code) {
			case -1:
				return ret_code;
			}
		}
	}
}

static unsigned char kdbstate[40];
static atomic_t kgdb_detected;

static int mdb_notify(struct notifier_block *nb, unsigned long reason,
		      void *data)
{
	register struct die_args *args = (struct die_args *)data;
	register unsigned long cr3;
	register int err = 0;

	if (atomic_read(&kgdb_detected))
		return NOTIFY_DONE;

	/* flush the tlb in case we are inside of a memory remap routine */
	cr3 = dbg_read_cr3();
	dbg_write_cr3(cr3);

	if (args) {
		switch (reason) {
		case DIE_DIE:
		case DIE_PANIC:
		case DIE_OOPS:
			mdb_oops = (unsigned char *)args->str;
			if (args->regs)
				err = mdb(SOFTWARE_EXCEPTION, args->err,
					  args->regs);
			else {
				struct pt_regs *regs = get_irq_regs();

				if (regs) {
					err = mdb(SOFTWARE_EXCEPTION,
						  args->err, regs);
					break;
				}

				/* if there are no regs passed on DIE_PANIC,
				 *  or we cannot locate a local interrupt
				 *  context, trigger an int 3 breakpoint and
				 *  get the register context since we were
				 *  apparently called from panic() outside
				 *  of an exception.
				 */
				mdb_breakpoint();
			}
			break;

		case DIE_INT3:
			if (toggle_user_break) {
				if (user_mode(args->regs))
					return NOTIFY_DONE;
			}
			err = mdb(BREAKPOINT_EXCEPTION, args->err, args->regs);
			break;

		case DIE_DEBUG:
			if (toggle_user_break) {
				if (user_mode(args->regs))
					return NOTIFY_DONE;
				if (test_thread_flag(TIF_SINGLESTEP))
					return NOTIFY_DONE;
			}
			err = mdb(DEBUGGER_EXCEPTION, args->err, args->regs);
			break;

		case DIE_NMI:
		case DIE_NMIUNKNOWN:
			err = mdb(NMI_EXCEPTION, args->err, args->regs);
			break;

		case DIE_CALL:
			err = mdb(KEYBOARD_ENTRY, args->err, args->regs);
			break;

		case DIE_KERNELDEBUG:
			err = mdb(KEYBOARD_ENTRY, args->err, args->regs);
			break;

		case DIE_GPF:
			err = mdb(GENERAL_PROTECTION, args->err, args->regs);
			break;

		case DIE_PAGE_FAULT:
			err = mdb(PAGE_FAULT_EXCEPTION, args->err, args->regs);
			break;

		default:
			break;
		}
	}
	mdb_watchdogs();
	return NOTIFY_STOP;
}

static struct notifier_block mdb_notifier = {
	.notifier_call = mdb_notify,
	.priority = 0x7FFFFFFF,
};

#if IS_ENABLED(CONFIG_MAGIC_SYSRQ)
static void sysrq_mdb(int key)
{
	mdb_breakpoint();
}

static int debug_previous_nmi[NR_CPUS];

static int mdb_nmi_handler(unsigned int cmd, struct pt_regs *regs)
{
	unsigned long processor = get_processor_id();

	if (atomic_read(&kgdb_detected))
		return NMI_DONE;

	switch (cmd) {
	case NMI_LOCAL:
		if (atomic_read(&inmdb) && is_processor_held(processor)) {
			if (!atomic_read(&inmdb_processor[processor])) {
				mdb(NMI_EXCEPTION, 0, regs);
				debug_previous_nmi[processor] = 1;
				mdb_watchdogs();
				return NMI_HANDLED;
			}
		}
		break;

	case NMI_UNKNOWN:
		if (debug_previous_nmi[processor]) {
			debug_previous_nmi[processor] = 0;
			return NMI_HANDLED;
		}
		break;

	default:
		break;
	}
	return NMI_DONE;
}

static struct sysrq_key_op sysrq_op = {
	.handler     = sysrq_mdb,
	.help_msg    = "mdb(g)",
	.action_msg  = "MDB",
};
#endif

static inline void strip_crlf(char *p, int limit)
{
	while (*p && limit) {
		if (*p == '\n' || (*p) == '\r')
			*p = '\0';
		p++;
		limit--;
	}
}

static ssize_t mdb_kernel_read(struct file *file, char *buf,
			       size_t count, loff_t offset)
{
	mm_segment_t old_fs;
	loff_t pos = offset;
	ssize_t res;

	old_fs = get_fs();
	set_fs(get_ds());
	res = vfs_read(file, (char __user *)buf, count, &pos);
	set_fs(old_fs);
	return res;
}

static ssize_t mdb_kernel_write(struct file *file, const char *buf,
				size_t count, loff_t pos)
{
	mm_segment_t old_fs;
	ssize_t res;

	old_fs = get_fs();
	set_fs(get_ds());
	res = vfs_write(file, (const char __user *)buf, count, &pos);
	set_fs(old_fs);

	return res;
}

static int __init mdb_init_module(void)
{
	register int i;
	register ssize_t size;
	register int ret = 0;
	struct file *filp;

	/* return if debugger already initialized */
	if (debugger_initialized)
		return 0;

	/* disable kgdb/kdb on module load if enabled */
	filp = filp_open("/sys/module/kgdboc/parameters/kgdboc", O_RDWR, 0);
	if (!IS_ERR(filp)) {
		size = mdb_kernel_read(filp, kdbstate, 39, 0);
		if (size) {
			strip_crlf(kdbstate, 39);

			pr_warn("MDB:  kgdb currently set to[%s], disabling.\n",
				kdbstate);

			kdbstate[0] = '\0';
			size = mdb_kernel_write(filp, kdbstate, 1, 0);
			if (!size) {
				pr_warn("MDB:  kgdb active, MDB disabled.\n");
				atomic_inc(&kgdb_detected);
			} else {
				pr_warn("MDB:  kgdb disabled. MDB enabled.\n");
			}
		}
		filp_close(filp, NULL);
	}

	initialize_debugger();

	ret = register_die_notifier(&mdb_notifier);
	if (ret) {
		clear_debugger_state();
		return ret;
	}

	ret = register_nmi_handler(NMI_LOCAL, mdb_nmi_handler, 0, "mdb");
	if (ret) {
		unregister_die_notifier(&mdb_notifier);
		clear_debugger_state();
		return ret;
	}

	ret = register_nmi_handler(NMI_UNKNOWN, mdb_nmi_handler, 0, "mdb");
	if (ret) {
		unregister_die_notifier(&mdb_notifier);
		unregister_nmi_handler(NMI_LOCAL, "mdb");
		clear_debugger_state();
		return ret;
	}

#if IS_ENABLED(CONFIG_MDB_DIRECT_MODE)
	disable_hw_bp_interface = 1;
#endif

#if IS_ENABLED(CONFIG_MAGIC_SYSRQ)
	register_sysrq_key('g', &sysrq_op);
#endif

	/* initialize delimiter lookup table */
	for (i = 0; i < 256; i++)
		delim_table[i] = '\0';

	set_delimiter('\0');
	set_delimiter('\n');
	set_delimiter('\r');
	set_delimiter('\t');
	set_delimiter('[');
	set_delimiter(']');
	set_delimiter('<');
	set_delimiter('>');
	set_delimiter('(');
	set_delimiter(')');
	set_delimiter('|');
	set_delimiter('&');
	set_delimiter('=');
	set_delimiter('*');
	set_delimiter('+');
	set_delimiter('-');
	set_delimiter('/');
	set_delimiter('%');
	set_delimiter('~');
	set_delimiter('^');
	set_delimiter('!');
	set_delimiter(' ');

	return 0;
}

static void __exit mdb_exit_module(void)
{
	del_timer(&debug_timer);
	debug_timer.data = 0;

#if IS_ENABLED(CONFIG_MAGIC_SYSRQ)
	unregister_sysrq_key('g', &sysrq_op);
#endif

#if IS_ENABLED(CONFIG_MDB_DIRECT_MODE)
	disable_hw_bp_interface = 0;
#endif

	unregister_die_notifier(&mdb_notifier);
	unregister_nmi_handler(NMI_UNKNOWN, "mdb");
	unregister_nmi_handler(NMI_LOCAL, "mdb");

	clear_debugger_state();
}

module_init(mdb_init_module);
module_exit(mdb_exit_module);

MODULE_DESCRIPTION("Minimal Kernel Debugger");
MODULE_LICENSE("GPL");
