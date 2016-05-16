
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
#include <linux/nmi.h>
#include <linux/delay.h>
#include <asm/segment.h>
#include <linux/atomic.h>
#include <asm/msr.h>
#include <linux/io.h>
#include <linux/clocksource.h>

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

#if IS_ENABLED(CONFIG_SMP) && !IS_ENABLED(CONFIG_X86_ELAN)

void dump_ioapic(unsigned long num)
{
	unsigned long i, val;

	if (num < nr_ioapics) {
		dbg_pr("io_apic registers\n");
		for (i = 0; i <= 0x2F; i++) {
			if ((i & 3) == 0)
				dbg_pr("%08X: ", i);

			val = native_io_apic_read(num, i * 4);
			dbg_pr("%08X ", val);

			if ((i & 3) == 3)
				dbg_pr("\n");
		}
	}
}

void dump_local_apic(void)
{
	unsigned long i, val;

	dbg_pr("local apic registers\n");
	for (i = 0; i <= 0x3F; i++) {
		if ((i & 3) == 0)
			dbg_pr("%08X: ", i);

		val = apic_read(i * 4);
		dbg_pr("%08X ", val);

		if ((i & 3) == 3)
			dbg_pr("\n");
	}
}

void dump_remote_apic(unsigned long cpu)
{
	register unsigned long i, timeout, apicid;
	register unsigned long val;

	dbg_pr("remote apic registers processor(%d)\n", cpu);
	for (i = 0; i <= 0x3F; i++) {
		if ((i & 3) == 0)
			dbg_pr("%08X: ", i);

		apicid = apic->cpu_present_to_apicid(cpu);
		if (apicid == BAD_APICID) {
			dbg_pr("BADAPICX ");
			continue;
		}

		timeout = 0;
		while (apic_read(APIC_ICR) & APIC_ICR_BUSY) {
			mdelay(100);
			if (timeout++ >= 1000)
				break;
			cpu_relax();
			mdb_watchdogs();
		}

		if (timeout >= 1000) {
			dbg_pr("???????? ");
			continue;
		}

		apic_write(APIC_ICR2, SET_APIC_DEST_FIELD(apicid));
		apic_write(APIC_ICR, i | APIC_DEST_LOGICAL | APIC_DM_REMRD);

		timeout = 0;
		while ((apic_read(APIC_ICR) & APIC_ICR_RR_MASK) ==
		       APIC_ICR_RR_INPROG) {
			mdelay(100);
			if (timeout++ >= 1000)
				break;

			cpu_relax();
			mdb_watchdogs();
		}

		if (timeout >= 1000) {
			dbg_pr("???????? ");
			continue;
		}

		if ((apic_read(APIC_ICR) & APIC_ICR_RR_MASK) ==
		    APIC_ICR_RR_VALID) {
			val = apic_read(APIC_RRR);
			dbg_pr("%08X ", val);
		} else {
			dbg_pr("???????? ");
		}

		if ((i & 3) == 3)
			dbg_pr("\n");
	}
}

unsigned long display_apic_help(unsigned char *command_line,
				dbg_parser *parser)
{
	dbg_pr("apic                    - display local apic regs\n");
	dbg_pr("apic[p#]                - display remote apic regs\n");
	return 1;
}

/* APIC */

unsigned long display_apic_info(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser)
{
	register unsigned long value;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid && ((value >= MAX_PROCESSORS) || !cpu_online(value))) {
		dbg_pr("processor not found\n");
		return 1;
	}
	if (valid && (value != get_processor_id()))
		dump_remote_apic(value);
	else
		dump_local_apic();
	return 1;
}

unsigned long display_ioapic_help(unsigned char *command_line,
				  dbg_parser *parser)
{
	dbg_pr("ioapic[#]               - display ioapic[#] regs\n");
	return 1;
}

/* IOAPIC */

unsigned long display_ioapic_info(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser)
{
	register unsigned long value;
	unsigned long valid;

	cmd = &cmd[parser->length];
	while (*cmd && *cmd == ' ')
		cmd++;

	value = eval_expr(dbgframe, &cmd, &valid);
	if (valid && !(value < nr_ioapics)) {
		dbg_pr("ioapic not found\n");
		return 1;
	}
	if (valid)
		dump_ioapic(value);
	else
		dump_ioapic(0);

	return 1;
}

#endif /* CONFIG_SMP */
