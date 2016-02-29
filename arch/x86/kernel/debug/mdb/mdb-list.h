
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

#ifndef _MDB_LIST
#define _MDB_LIST

struct accel_key {
	struct accel_key *next;
	struct accel_key *prior;
	unsigned long (*accel)(unsigned long key, void *p,
			       struct accel_key *parser);
	unsigned long (*accel_help)(unsigned long key,
				    struct accel_key *parser);
	unsigned long flags;
	unsigned long key;
	unsigned long super;
	unsigned char *help;
};

struct alt_dbg {
	struct alt_dbg *next;
	struct alt_dbg *prior;
	int (*altdbg)(int reason, int error, void *frame);
};

struct dbg_parser {
	struct dbg_parser *next;
	struct dbg_parser *prior;
	unsigned long (*parser)(unsigned char *command_line,
				dbg_regs *dbgframe,
				unsigned long exception,
				struct dbg_parser *parser);
	unsigned long (*parser_help)(unsigned char *command_line,
				     struct dbg_parser *parser);
	unsigned long flags;
	unsigned char *name;
	unsigned long length;
	unsigned long super;
	unsigned char *help;
	unsigned long xfer;
	unsigned long category;
};

struct dbg_list {
	struct dbg_parser *head;
	struct dbg_parser *tail;
};

#define accel_key   struct accel_key
#define alt_dbg     struct alt_dbg
#define dbg_parser  struct dbg_parser
#define dbg_list    struct dbg_list

int altdbg_routine(int reason, int error, void *frame);
unsigned long add_alt_dbg(alt_dbg *dbg);
unsigned long remove_alt_dbg(alt_dbg *dbg);
unsigned long debugger_parser_routine(unsigned char *command,
				      unsigned char *command_line,
				      dbg_regs *dbgframe,
				      unsigned long exception);
unsigned long debugger_parser_help_routine(unsigned char *command,
					   unsigned char *command_line);
unsigned long add_debug_parser(dbg_parser *parser);
unsigned long remove_debug_parser(dbg_parser *parser);

static inline unsigned long strhash(unsigned char *s, int len, int limit)
{
	register unsigned long h = 0, a = 127, i;

	if (!limit)
		return -1;

	for (i = 0; i < len && *s; s++, i++)
		h = ((a * h) + *s) % limit;

	return h;
}

#endif
