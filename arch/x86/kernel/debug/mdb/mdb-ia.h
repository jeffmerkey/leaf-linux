
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

#ifndef _MDB_IA32_H
#define _MDB_IA32_H

#define MAX_PROCESSORS           NR_CPUS
#define SYMBOL_DEBUG             0

/* recursive spin lock used by the debugger to gate processors acquiring
 * the debugger console.  if the lock is already held on the current
 * processor when called, increment a use counter.  this allows us to
 * handle nested exceptions on the same processor without deadlocking.
 */

struct _RLOCK {
	unsigned long processor;
	unsigned long count;
	unsigned long flags[MAX_PROCESSORS];
};

#define rlock_t struct _RLOCK

#if IS_ENABLED(CONFIG_X86_64)
struct _dbg_regs {
	unsigned long t_res[7];
	unsigned long t_cr3;
	unsigned long t_ip;
	unsigned long t_flags;
	unsigned long t_ax;
	unsigned long t_cx;
	unsigned long t_dx;
	unsigned long t_bx;
	unsigned long t_sp;
	unsigned long t_bp;
	unsigned long t_si;
	unsigned long t_di;
	unsigned long t_es;
	unsigned long t_cs;
	unsigned long t_ss;
	unsigned long t_ds;
	unsigned long t_fs;
	unsigned long t_gs;
	unsigned long t_ldt;
	unsigned long t_io_map;
	unsigned long r8;
	unsigned long r9;
	unsigned long r10;
	unsigned long r11;
	unsigned long r12;
	unsigned long r13;
	unsigned long r14;
	unsigned long r15;
};
#else
struct _dbg_regs {
	unsigned long t_res[7];
	unsigned long t_cr3;
	unsigned long t_ip;
	unsigned long t_flags;
	unsigned long t_ax;
	unsigned long t_cx;
	unsigned long t_dx;
	unsigned long t_bx;
	unsigned long t_sp;
	unsigned long t_bp;
	unsigned long t_si;
	unsigned long t_di;
	unsigned long t_es;
	unsigned long t_cs;
	unsigned long t_ss;
	unsigned long t_ds;
	unsigned long t_fs;
	unsigned long t_gs;
	unsigned long t_ldt;
	unsigned long t_io_map;
};
#endif

#define dbg_regs struct _dbg_regs

/*  128 bytes total size numeric register context */

struct NPXREG {
	u16 sig0;        /*  10 bytes total size this structure */
	u16 sig1;
	u16 sig2;
	u16 sig3;
	u16 exponent:15;
	u16 sign:1;
};

struct NPX {
	u32 control;
	u32 status;
	u32 tag;
	u32 eip;
	u32 cs;
	u32 dataptr;
	u32 datasel;
	struct NPXREG reg[8];    /* 80 bytes */
	u32 pad[5];
};

#define NUMERIC_FRAME struct NPX

/*  128 bytes total size register context */

static inline void _cli(void)
{
	__asm__ __volatile__("cli" : : : "memory");
}

static inline void _sti(void)
{
	__asm__ __volatile__("sti" : : : "memory");
}

#if IS_ENABLED(CONFIG_X86_64)
static inline unsigned long get_flags(void)
{
	unsigned long flags;

	__asm__ __volatile__(
			     "pushfq ; popq %0"
			     : "=g" (flags)
			     :
			    );
	return flags;
}

static inline unsigned long save_flags(void)
{
	unsigned long flags;

	__asm__ __volatile__(
			     "pushfq ; popq %0"
			     : "=g" (flags)
			     :
			    );
	__asm__ __volatile__("cli" : : : "memory");
	return flags;
}

static inline void restore_flags(unsigned long flags)
{
	__asm__ __volatile__(
			     "pushq %0 ; popfq"
			     :
			     : "g" (flags)
			     : "memory", "cc"
			    );
}
#else
static inline unsigned long get_flags(void)
{
	unsigned long flags;

	__asm__ __volatile__(
			     "pushfl ; popl %0"
			     : "=g" (flags)
			     :
			    );
	return flags;
}

static inline unsigned long save_flags(void)
{
	unsigned long flags;

	__asm__ __volatile__(
			     "pushfl ; popl %0"
			     : "=g" (flags)
			     :
			    );
	__asm__ __volatile__("cli" : : : "memory");
	return flags;
}

static inline void restore_flags(unsigned long flags)
{
	__asm__ __volatile__(
			     "pushl %0 ; popfl"
			     :
			     : "g" (flags)
			     : "memory", "cc"
			    );
}

#endif

#endif
