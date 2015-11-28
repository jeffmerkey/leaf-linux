
/***************************************************************************
*
*   Copyright (c) 2008 Jeff V. Merkey  All Rights Reserved.
*   1058 East 50 South
*   Lindon, Utah 84042
*   jmerkey@wolfmountaingroup.com
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
*   jmerkey@wolfmountaingroup.com is the official maintainer of
*   this code.  You are encouraged to report any bugs, problems, fixes,
*   suggestions, and comments about this software.
*
*   AUTHOR   :  Jeff V. Merkey
*   DESCRIP  :  Merkey's Linux Debugger
*
***************************************************************************/

#ifndef _MDB_IA32_H
#define _MDB_IA32_H

typedef struct _StackFrame {
   unsigned long tReserved[7];
   unsigned long *tCR3;
   unsigned long tEIP;
   unsigned long tSystemFlags;
   unsigned long tEAX;
   unsigned long tECX;
   unsigned long tEDX;
   unsigned long tEBX;
   unsigned long tESP;
   unsigned long tEBP;
   unsigned long tESI;
   unsigned long tEDI;
   unsigned short tES;
   unsigned short Res1;
   unsigned short tCS;
   unsigned short Res2;
   unsigned short tSS;
   unsigned short Res3;
   unsigned short tDS;
   unsigned short Res4;
   unsigned short tFS;
   unsigned short Res5;
   unsigned short tGS;
   unsigned short Res6;
   unsigned short tLDT;
   unsigned short Res7;
   unsigned long tIOMap;
} StackFrame;

/*  128 bytes total size numeric register context */

typedef struct _NPXREG {
  unsigned short sig0;        /*  10 bytes total size this structure */
  unsigned short sig1;
  unsigned short sig2;
  unsigned short sig3;
  unsigned short exponent:15;
  unsigned short sign:1;
} NUMERIC_REGISTER_CONTEXT;

typedef struct _NPX {
  unsigned long control;
  unsigned long status;
  unsigned long tag;
  unsigned long eip;
  unsigned long cs;
  unsigned long dataptr;
  unsigned long datasel;
  NUMERIC_REGISTER_CONTEXT reg[8];    /* 80 bytes */
  unsigned long pad[5];
} NUMERIC_FRAME;

/*  128 bytes total size register context */

typedef struct _CONTEXT_FRAME {
    unsigned short cBackLink;
    unsigned short cTSSReserved;
    unsigned long cESP0;
    unsigned short cSS0;
    unsigned short cSS0res;
    unsigned long cESP1;
    unsigned short cSS1;
    unsigned short cSS1res;
    unsigned long cESP2;
    unsigned short cSS2;
    unsigned short cSS2res;
    unsigned long cCR3;
    unsigned long cEIP;
    unsigned long cSystemFlags;
    unsigned long cEAX;
    unsigned long cECX;
    unsigned long cEDX;
    unsigned long cEBX;
    unsigned long cESP;
    unsigned long cEBP;
    unsigned long cESI;
    unsigned long cEDI;
    unsigned long cES;
    unsigned long cCS;
    unsigned long cSS;
    unsigned long cDS;
    unsigned long cFS;
    unsigned long cGS;
    unsigned long cLDT;
    unsigned long cIOPermissMap;
    unsigned long pad[6];
} CONTEXT_FRAME;

static inline void _cli(void)
{
	__asm__ __volatile__("cli" : : : "memory");
}

static inline void _sti(void)
{
	__asm__ __volatile__("sti" : : : "memory");
}

static inline unsigned long get_flags(void)
{
	unsigned long flags;

	__asm__ __volatile__(
		"pushfl ; popl %0"
		: "=g" (flags)
		: /* no input */
	);
	return flags;
}

static inline unsigned long save_flags(void)
{
	unsigned long flags;

	__asm__ __volatile__(
		"pushfl ; popl %0"
		: "=g" (flags)
		: /* no input */
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
