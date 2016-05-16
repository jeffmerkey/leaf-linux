
/***************************************************************************
 *
 *   Copyright (c) 2000-2016 Jeff V. Merkey  All Rights Reserved.
 *   jeffmerkey@gmail.com
 *
 *   This program is free software; you can redistribute it and/or modify it
 *   under the terms of the GNU General Public License as published by the
 *   Free Software Foundation,  version 2.
 *
 *   This program is distributed in the hope that it will be useful,  but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   AUTHOR   :   Jeff V. Merkey
 *   DESCRIP  :   Minimal Linux Debugger
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

accel_key *accel_head;
accel_key *accel_tail;

unsigned long accel_routine(unsigned long key,  void *p)
{
	register accel_key *accel;
	unsigned long ret_code;

	accel = accel_head;
	while (accel) {
		if (accel->flags && accel->accel &&
		    accel->key == key) {
			ret_code = accel->accel(key, p, accel);
			return ret_code;
		}
		accel = accel->next;
	}
	return 0;
}

void show_debugger_accelerators(void)
{
	register accel_key *accel;

	dbg_pr("\nDebugger Keystroke Accelerator(s)\n");

	accel = accel_head;
	while (accel) {
		if (accel->help)
			if (dbg_pr("%s\n", accel->help))
				return;
		accel = accel->next;
	}
}

unsigned long is_accelerator(unsigned long key)
{
	register accel_key *accel;

	accel = accel_head;
	while (accel) {
		if (accel->flags && accel->accel &&
		    accel->key == key)
			return 1;
		accel = accel->next;
	}
	return 0;
}

unsigned long accel_help_routine(unsigned long key)
{
	register accel_key *accel;

	accel = accel_head;
	if (key)  /* if we were passed a command string */ {
		while (accel) {
			if (accel->flags && accel->key == key) {
				if (accel->accel_help) {
					dbg_pr("Accelerator %08X\n",
					       accel->key);
					accel->accel_help(key, accel);
					return 1;
				}
				dbg_pr("Accelerator %08X\n",
				       accel->key);
				return 1;
			}
			accel = accel->next;
		}
		dbg_pr("Help for Accelerator[%08X] not found\n",
		       key);
		return 1;
	}

	dbg_pr("Accelerator(s)\n");
	while (accel) {
		if (accel->flags && accel->key &&
		    !accel->super)
			dbg_pr("%08X         - %s\n",
			       accel->key,
			       accel->help);
		accel = accel->next;
	}
	return 0;
}

accel_key *insert_accel(accel_key *i,  accel_key *top)
{
	accel_key *old,  *p;

	if (!accel_tail) {
		i->next = NULL;
		i->prior = NULL;
		accel_tail = i;
		return i;
	}
	p = top;
	old = NULL;
	while (p) {
		if (p->key < i->key) {
			old = p;
			p = p->next;
		} else {
			if (p->prior) {
				p->prior->next = i;
				i->next = p;
				i->prior = p->prior;
				p->prior = i;
				return top;
			}
			i->next = p;
			i->prior = NULL;
			p->prior = i;
			return i;
		}
	}
	old->next = i;
	i->next = NULL;
	i->prior = old;
	accel_tail = i;
	return accel_head;
}

unsigned long add_accel_routine(accel_key *new)
{
	register accel_key *accel;

	accel = accel_head;
	while (accel) {
		if (accel == new || accel->key == new->key)
			return 1;
		accel = accel->next;
	}
	new->flags = -1;
	accel_head = insert_accel(new,  accel_head);

	return 0;
}

unsigned long remove_accel_routine(accel_key *new)
{
	register accel_key *accel;

	accel = accel_head;
	while (accel) {
		if (accel == new)   /* found,  remove from list */ {
			if (accel_head == new) {
				accel_head = (void *)new->next;
				if (accel_head)
					accel_head->prior = NULL;
				else
					accel_tail = NULL;
			} else {
				new->prior->next =
					new->next;
				if (new != accel_tail)
					new->next->prior =
						new->prior;
				else
					accel_tail = new->prior;
			}
			new->next = 0;
			new->prior = 0;
			new->flags = 0;

			return 0;
		}
		accel = accel->next;
	}

	return -1;
}

alt_dbg *alt_head;
alt_dbg *alt_tail;

int altdbg_routine(int reason,  int error,  void *frame)
{
	register alt_dbg *alt_debug;
	register unsigned long ret_code;
	unsigned long state;

	state = save_flags();
	alt_debug = alt_head;
	while (alt_debug) {
		if (alt_debug->altdbg) {
			ret_code = alt_debug->altdbg(reason,  error,
						     frame);
			if (ret_code) {
				restore_flags(state);
				return ret_code;
			}
		}
		alt_debug = alt_debug->next;
	}
	restore_flags(state);
	return 0;
}

unsigned long add_alt_dbg(alt_dbg *alt)
{
	register alt_dbg *alt_debug;

	alt_debug = alt_head;
	while (alt_debug) {
		if (alt_debug == alt)
			return 1;
		alt_debug = alt_debug->next;
	}
	if (!alt_head) {
		alt_head = alt;
		alt_tail = alt;
		alt->next = 0;
		alt->prior = 0;
	} else {
		alt_tail->next = alt;
		alt->next = 0;
		alt->prior = alt_tail;
		alt_tail = alt;
	}

	return 0;
}

unsigned long remove_alt_dbg(alt_dbg *alt)
{
	register alt_dbg *alt_debug;

	alt_debug = alt_head;
	while (alt_debug) {
		if (alt_debug == alt)   /* found,  remove from list */ {
			if (alt_head == alt) {
				alt_head = (void *)alt->next;
				if (alt_head)
					alt_head->prior = NULL;
				else
					alt_tail = NULL;
			} else {
				alt->prior->next = alt->next;
				if (alt != alt_tail)
					alt->next->prior = alt->prior;
				else
					alt_tail = alt->prior;
			}
			alt->next = 0;
			alt->prior = 0;

			return 0;
		}
		alt_debug = alt_debug->next;
	}
	return -1;
}

dbg_parser *dbg_head;
dbg_parser *dbg_tail;

unsigned long debugger_parser_routine(unsigned char *command,
				      unsigned char *command_line,
				      dbg_regs *dbgframe,
				      unsigned long exception)
{
	register dbg_parser *d_parser;
	register unsigned long ret_code,  valid = 0,  length;
	register unsigned char *p;

	p = command_line;
	if (!p)
		return 0;

	/* if a passed string is just whitespace,  return error */
	while (*p) {
		if ((*p != ' ') && (*p != '\n') && (*p != '\r')) {
			valid = 1;
			break;
		}
		p++;
	}
	if (!valid)
		return 0;

	upcase_string(command);
	length = strlen(command);

	d_parser = dbg_head;
	while (d_parser) {
		if (d_parser->flags && d_parser->parser &&
		    (d_parser->length == length) &&
		    (!strcmp(d_parser->name, command))) {
			ret_code =
				d_parser->parser(command_line,
						 dbgframe,
						 exception,
						 d_parser);
			if (ret_code)
				return ret_code;
		}
		d_parser = d_parser->next;
	}

	dbg_pr("unknown mdb command -> %s\n",  command);
	return 0;
}

void strncpy2lower(unsigned char *dest,  unsigned char *src,  int len)
{
	register int i;

	for (i = 0; (i + 1) < len && *src; i++)
		*dest++ = tolower(*src++);
	*dest = '\0';
}

unsigned long debugger_parser_help_routine(unsigned char *command,
					   unsigned char *command_line)
{
	register dbg_parser *d_parser;
	register unsigned long length;
	register int i;

	upcase_string(command);
	length = strlen(command);
	/* if we were passed a command string */
	if (*command) {
		d_parser = dbg_head;
		while (d_parser) {
			if (d_parser->flags &&
			    (d_parser->length == length) &&
			    !strcmp(d_parser->name,  command)) {
				if (d_parser->parser_help) {
					dbg_pr("Help for Command %s\n",
					       d_parser->name);
					d_parser->parser_help(command_line,
							      d_parser);
					return 1;
				}
				dbg_pr("Help for Command %s\n",
				       d_parser->name);
				return 1;
			}
			d_parser = d_parser->next;
		}

		dbg_pr("Help for Command[%s] not found\n",  command);
		return 1;
	}

	dbg_pr("Debugger Command(s)\n");
	dbg_pr("HELP         <enter> - list all commands\n");
	dbg_pr("HELP command <enter> - help for specific command\n");

	for (i = 0; i < 13; i++) {
		if (!category_strings[i])
			break;

		if (dbg_pr("\n %s\n",  category_strings[i]))
			return 0;

		d_parser = dbg_head;
		while (d_parser) {
			unsigned char debug_temp[256];

			if (d_parser->flags &&
			    d_parser->name &&
			    !d_parser->super &&
			    d_parser->category == i) {
				strncpy2lower(debug_temp, d_parser->name,
					      256);
				if (dbg_pr("  %-10s    - %s\n",
					   debug_temp, d_parser->help))
					return 0;
			}
			d_parser = d_parser->next;
		}
	}
	show_debugger_accelerators();
	return 0;
}

dbg_parser *insert_debugger_parser(dbg_parser *i,  dbg_parser *top)
{
	dbg_parser *old,  *p;

	if (!dbg_tail) {
		i->next = NULL;
		i->prior = NULL;
		dbg_tail = i;
		return i;
	}
	p = top;
	old = NULL;
	while (p) {
		if (strcmp(p->name,  i->name) < 0) {
			old = p;
			p = p->next;
		} else {
			if (p->prior) {
				p->prior->next = i;
				i->next = p;
				i->prior = p->prior;
				p->prior = i;
				return top;
			}
			i->next = p;
			i->prior = NULL;
			p->prior = i;
			return i;
		}
	}
	old->next = i;
	i->next = NULL;
	i->prior = old;
	dbg_tail = i;
	return dbg_head;
}

unsigned long add_debug_parser(dbg_parser *parser)
{
	register dbg_parser *d_parser;

	parser->flags = -1;
	parser->length = strlen(parser->name);

	d_parser = dbg_head;
	while (d_parser) {
		if (d_parser == parser ||
		    (parser->length ==
		     d_parser->length &&
		     (!strcasecmp(parser->name,
				  d_parser->name))))
			return 1;
		d_parser = d_parser->next;
	}
	dbg_head = insert_debugger_parser(parser,  dbg_head);

	return 0;
}

unsigned long remove_debug_parser(dbg_parser *parser)
{
	register dbg_parser *d_parser;

	d_parser = dbg_head;
	while (d_parser) {
		if (d_parser == parser) {
			/* found,  remove from list */
			if (dbg_head == parser) {
				dbg_head = (void *)parser->next;
				if (dbg_head)
					dbg_head->prior = NULL;
				else
					dbg_tail = NULL;
			} else {
				parser->prior->next = parser->next;
				if (parser != dbg_tail)
					parser->next->prior = parser->prior;
				else
					dbg_tail = parser->prior;
			}
			parser->next = 0;
			parser->prior = 0;
			parser->flags = 0;

			return 0;
		}
		d_parser = d_parser->next;
	}
	return -1;
}
