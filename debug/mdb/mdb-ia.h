
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

#ifndef _MDB_IA32_H
#define _MDB_IA32_H

#define MAX_PROCESSORS           NR_CPUS
#define SYMBOL_DEBUG             0

/* recursive spin lock used by the debugger to gate processors acquiring
 * the debugger console.  if the lock is already held on the current
 * processor when called, increment a use counter.  this allows us to
 * handle nested exceptions on the same processor without deadlocking */
typedef struct _RLOCK
{
#if defined(CONFIG_SMP)
    spinlock_t lock;
#endif
    unsigned long processor;
    unsigned long count;
    unsigned long flags[MAX_PROCESSORS];
} rlock_t;

#ifdef CONFIG_X86_64
typedef struct _StackFrame {
   unsigned long tReserved[7];
   unsigned long tCR3;
   unsigned long tIP;
   unsigned long tSystemFlags;
   unsigned long tAX;
   unsigned long tCX;
   unsigned long tDX;
   unsigned long tBX;
   unsigned long tSP;
   unsigned long tBP;
   unsigned long tSI;
   unsigned long tDI;
   unsigned long tES;
   unsigned long tCS;
   unsigned long tSS;
   unsigned long tDS;
   unsigned long tFS;
   unsigned long tGS;
   unsigned long tLDT;
   unsigned long tIOMap;
   unsigned long r8;
   unsigned long r9;
   unsigned long r10;
   unsigned long r11;
   unsigned long r12;
   unsigned long r13;
   unsigned long r14;
   unsigned long r15;
} StackFrame;
#else
typedef struct _StackFrame {
   unsigned long tReserved[7];
   unsigned long tCR3;
   unsigned long tIP;
   unsigned long tSystemFlags;
   unsigned long tAX;
   unsigned long tCX;
   unsigned long tDX;
   unsigned long tBX;
   unsigned long tSP;
   unsigned long tBP;
   unsigned long tSI;
   unsigned long tDI;
   unsigned long tES;
   unsigned long tCS;
   unsigned long tSS;
   unsigned long tDS;
   unsigned long tFS;
   unsigned long tGS;
   unsigned long tLDT;
   unsigned long tIOMap;
} StackFrame;
#endif

/*  128 bytes total size numeric register context */

typedef struct _NPXREG {
  uint16_t sig0;        /*  10 bytes total size this structure */
  uint16_t sig1;
  uint16_t sig2;
  uint16_t sig3;
  uint16_t exponent:15;
  uint16_t sign:1;
} NUMERIC_REGISTER_CONTEXT;

typedef struct _NPX {
  uint32_t control;
  uint32_t status;
  uint32_t tag;
  uint32_t eip;
  uint32_t cs;
  uint32_t dataptr;
  uint32_t datasel;
  NUMERIC_REGISTER_CONTEXT reg[8];    /* 80 bytes */
  uint32_t pad[5];
} NUMERIC_FRAME;

/*  128 bytes total size register context */

static inline void _cli(void)
{
	__asm__ __volatile__("cli" : : : "memory");
}

static inline void _sti(void)
{
	__asm__ __volatile__("sti" : : : "memory");
}


#ifdef CONFIG_X86_64
static inline unsigned long get_flags(void)
{
	unsigned long flags;
	__asm__ __volatile__(
		"pushfq ; popq %0"
		: "=g" (flags)
		: // no input 
	);
	return flags;
}

static inline unsigned long save_flags(void)
{
	unsigned long flags;

	__asm__ __volatile__(
		"pushfq ; popq %0"
		: "=g" (flags)
		: // no input 
	);
	__asm__ __volatile__("cli" : : : "memory");
	return flags;
}

static inline void restore_flags(unsigned long flags)
{
	__asm__ __volatile__(
		"pushq %0 ; popfq"
		:
		:"g" (flags)
		:"memory", "cc"
	);
}
#else
static inline unsigned long get_flags(void)
{
	unsigned long flags;
	__asm__ __volatile__(
		"pushfl ; popl %0"
		: "=g" (flags)
		: // no input 
	);
	return flags;
}

static inline unsigned long save_flags(void)
{
	unsigned long flags;

	__asm__ __volatile__(
		"pushfl ; popl %0"
		: "=g" (flags)
		: // no input 
	);
	__asm__ __volatile__("cli" : : : "memory");
	return flags;
}

static inline void restore_flags(unsigned long flags)
{
	__asm__ __volatile__(
		"pushl %0 ; popfl"
		:
		:"g" (flags)
		:"memory", "cc"
	);
}

#endif

#endif
