
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
#include <linux/io.h>
#include <linux/kdebug.h>
#include <linux/notifier.h>
#include <linux/sysrq.h>
#include <linux/input.h>

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
#include "mdb-ia-support.h"

#define MAXLEN 20
#define abort()
#ifndef UNIXWARE_COMPAT
#define UNIXWARE_COMPAT 1
#endif

static int fetch_data(struct disassemble_info *, mdb_byte *);
static void ckprefix(void);
static const char *prefix_name(int, int);
static int print_insn(mdb_vma, struct disassemble_info *, dbg_regs *);
static void dofloat (int, dbg_regs *);
static void OP_ST(int, int, dbg_regs *);
static void OP_STI(int, int, dbg_regs *);
static int putop(const char *, int);
static void oappend(const char *);
static void append_seg(void);
static void OP_INDIRE(int, int, dbg_regs *);
static void print_operand_value(char *, int, mdb_vma);
static void OP_E(int, int, dbg_regs *);
static void OP_G(int, int, dbg_regs *);
static mdb_vma get64(void);
static mdb_signed_vma get32(void);
static mdb_signed_vma get32s(void);
static int get16(void);
static void set_op(mdb_vma, int);
static void OP_REG(int, int, dbg_regs *);
static void OP_IMREG(int, int, dbg_regs *);
static void OP_I(int, int, dbg_regs *);
static void OP_I64(int, int, dbg_regs *);
static void OP_SI(int, int, dbg_regs *);
static void OP_J(int, int, dbg_regs *);
static void OP_SEG(int, int, dbg_regs *);
static void OP_DIR(int, int, dbg_regs *);
static void OP_OFF(int, int, dbg_regs *);
static void OP_OFF64(int, int, dbg_regs *);
static void ptr_reg(int, int, dbg_regs *);
static void OP_ESREG(int, int, dbg_regs *);
static void OP_DSREG(int, int, dbg_regs *);
static void OP_C(int, int, dbg_regs *);
static void OP_D(int, int, dbg_regs *);
static void OP_T(int, int, dbg_regs *);
static void OP_RD(int, int, dbg_regs *);
static void OP_MMX(int, int, dbg_regs *);
static void OP_XMM(int, int, dbg_regs *);
static void OP_EM(int, int, dbg_regs *);
static void OP_EX(int, int, dbg_regs *);
static void OP_MS(int, int, dbg_regs *);
static void OP_XS(int, int, dbg_regs *);
static void OP_M(int, int, dbg_regs *);
static void OP_VMX(int, int, dbg_regs *);
static void OP_0FAE(int, int, dbg_regs *);
static void OP_0F07(int, int, dbg_regs *);
static void NOP_FIXUP(int, int, dbg_regs *);
static void OP_3DNOWSUFFIX(int, int, dbg_regs *);
static void OP_SIMD_SUFFIX(int, int, dbg_regs *);
static void SIMD_FIXUP(int, int, dbg_regs *);
static void PNI_FIXUP(int, int, dbg_regs *);
static void SVME_FIXUP(int, int, dbg_regs *);
static void INVLPG_FIXUP(int, int, dbg_regs *);
static void bad_op(void);
static void SEG_FIXUP(int, int, dbg_regs *);
static void VMX_FIXUP(int, int, dbg_regs *);

struct dis_private {
	/* Points to first byte not fetched.	*/
	mdb_byte *max_fetched;
	mdb_byte the_buffer[MAXLEN];
	mdb_vma insn_start;
	int orig_sizeflag;
};

/* The opcode for the fwait instruction, which we
 * treat as a prefix when we can.
 */
#define FWAIT_OPCODE (0x9b)

/* Set to 1 for 64bit mode disassembly.	 */
static int mode_64bit;

/* Flags for the prefixes for the current instruction.*/
static int prefixes;

/* REX prefix the current instruction.	See below.  */
static int rex;
/* Bits of REX we've already used.  */
static int rex_used;
#define REX_MODE64	8
#define REX_EXTX	4
#define REX_EXTY	2
#define REX_EXTZ	1
/* Mark parts used in the REX prefix.  When we are testing for
 * empty prefix (for 8bit register REX extension), just mask it
 * out.	 Otherwise test for REX bit is excuse for existence of
 * REX only in case value is nonzero.
 */
#define USED_REX(value)					\
{							\
	if (value)						\
	rex_used |= (rex & value) ? (value) | 0x40 : 0;	\
	else						\
	rex_used |= 0x40;					\
}

/* Flags for prefixes which we somehow handled when printing
 * the current instruction.
 */
static int used_prefixes;

/* Flags stored in PREFIXES.  */
#define PREFIX_REPZ 1
#define PREFIX_REPNZ 2
#define PREFIX_LOCK 4
#define PREFIX_CS 8
#define PREFIX_SS 0x10
#define PREFIX_DS 0x20
#define PREFIX_ES 0x40
#define PREFIX_FS 0x80
#define PREFIX_GS 0x100
#define PREFIX_DATA 0x200
#define PREFIX_ADDR 0x400
#define PREFIX_FWAIT 0x800

/* Make sure that bytes from INFO->PRIVATE_DATA->BUFFER (inclusive)
 * to ADDR (exclusive) are valid.  Returns 1 for success.
 */
#define FETCH_DATA(info, addr) \
	((addr) <= ((struct dis_private *)(info->private_data))->max_fetched \
	 ? 1 : fetch_data((info), (addr)))

struct disassemble_info mdb_di;
char disbuf[512];
char fworkbuf[512];
char fbytebuf[512];
char finalbuf[512];
unsigned long needs_proceed;
unsigned long jmp_active;
unsigned long trap_disable;
unsigned long columns;
void *vaddr;

static int
fetch_data(struct disassemble_info *info, mdb_byte *addr)
{
	int status;
	struct dis_private *priv = info->private_data;
	mdb_vma start = priv->insn_start + (priv->max_fetched -
					    priv->the_buffer);

	status = (*info->read_memory_func) (start,
					    priv->max_fetched,
					    addr - priv->max_fetched,
					    info);
	if (status != 0) {
		/* If we did manage to read at least one byte, then
		 * print_insn_i386 will do something sensible.  Otherwise,
		 * print an error.  We do that here because this is
		 * where we know STATUS.
		 */
		if (priv->max_fetched == priv->the_buffer)
			(*info->memory_error_func) (status, start, info);

		mdb_printf("fetch data error\n");
	} else {
		priv->max_fetched = addr;
	}
	return 1;
}

/* bits in sizeflag */
#define SUFFIX_ALWAYS 4
#define AFLAG 2
#define DFLAG 1

#define b_mode 1  /* byte operand */
#define v_mode 2  /* operand size depends on prefixes */
#define w_mode 3  /* word operand */
#define d_mode 4  /* double word operand  */
#define q_mode 5  /* quad word operand */
#define t_mode 6  /* ten-byte operand */
#define x_mode 7  /* 16-byte XMM operand */
#define m_mode 8  /* d_mode in 32bit, q_mode in 64bit mode.  */
#define cond_jump_mode 9
#define loop_jcxz_mode 10
#define dq_mode 11 /* operand size depends on REX prefixes.  */
#define dqw_mode 12 /* registers like dq_mode, memory like w_mode.  */
#define f_mode 13 /* 4- or 6-byte pointer operand */
#define const_1_mode 14
#define branch_v_mode 15 /* v_mode for branch.	*/

#define es_reg 100
#define cs_reg 101
#define ss_reg 102
#define ds_reg 103
#define fs_reg 104
#define gs_reg 105

#define eax_reg 108
#define ecx_reg 109
#define edx_reg 110
#define ebx_reg 111
#define esp_reg 112
#define ebp_reg 113
#define esi_reg 114
#define edi_reg 115

#define al_reg 116
#define cl_reg 117
#define dl_reg 118
#define bl_reg 119
#define ah_reg 120
#define ch_reg 121
#define dh_reg 122
#define bh_reg 123

#define ax_reg 124
#define cx_reg 125
#define dx_reg 126
#define bx_reg 127
#define sp_reg 128
#define bp_reg 129
#define si_reg 130
#define di_reg 131

#define rax_reg 132
#define rcx_reg 133
#define rdx_reg 134
#define rbx_reg 135
#define rsp_reg 136
#define rbp_reg 137
#define rsi_reg 138
#define rdi_reg 139

#define indir_dx_reg 150

#define FLOATCODE 1
#define USE_GROUPS 2
#define USE_PREFIX_TABLE 3
#define X86_64_SPECIAL 4

#define FLOAT	  { NULL, NULL, FLOATCODE, NULL, 0, NULL,  0 }

#define GRP1B	  { NULL, NULL, USE_GROUPS, NULL, 0, NULL,  0 }
#define GRP1S	  { NULL, NULL, USE_GROUPS, NULL, 1, NULL,  0 }
#define GRP1SS	  { NULL, NULL, USE_GROUPS, NULL, 2, NULL,  0 }
#define GRP2B	  { NULL, NULL, USE_GROUPS, NULL, 3, NULL,  0 }
#define GRP2S	  { NULL, NULL, USE_GROUPS, NULL, 4, NULL,  0 }
#define GRP2B_ONE { NULL, NULL, USE_GROUPS, NULL, 5, NULL,  0 }
#define GRP2S_ONE { NULL, NULL, USE_GROUPS, NULL, 6, NULL,  0 }
#define GRP2B_CL  { NULL, NULL, USE_GROUPS, NULL, 7, NULL,  0 }
#define GRP2S_CL  { NULL, NULL, USE_GROUPS, NULL, 8, NULL,  0 }
#define GRP3B	  { NULL, NULL, USE_GROUPS, NULL, 9, NULL,  0 }
#define GRP3S	  { NULL, NULL, USE_GROUPS, NULL, 10, NULL,  0 }
#define GRP4	  { NULL, NULL, USE_GROUPS, NULL, 11, NULL,  0 }
#define GRP5	  { NULL, NULL, USE_GROUPS, NULL, 12, NULL,  0 }
#define GRP6	  { NULL, NULL, USE_GROUPS, NULL, 13, NULL,  0 }
#define GRP7	  { NULL, NULL, USE_GROUPS, NULL, 14, NULL,  0 }
#define GRP8	  { NULL, NULL, USE_GROUPS, NULL, 15, NULL,  0 }
#define GRP9	  { NULL, NULL, USE_GROUPS, NULL, 16, NULL,  0 }
#define GRP10	  { NULL, NULL, USE_GROUPS, NULL, 17, NULL,  0 }
#define GRP11	  { NULL, NULL, USE_GROUPS, NULL, 18, NULL,  0 }
#define GRP12	  { NULL, NULL, USE_GROUPS, NULL, 19, NULL,  0 }
#define GRP13	  { NULL, NULL, USE_GROUPS, NULL, 20, NULL,  0 }
#define GRP14	  { NULL, NULL, USE_GROUPS, NULL, 21, NULL,  0 }
#define GRPAMD	  { NULL, NULL, USE_GROUPS, NULL, 22, NULL,  0 }
#define GRPPADLCK1 { NULL, NULL, USE_GROUPS, NULL, 23, NULL,  0 }
#define GRPPADLCK2 { NULL, NULL, USE_GROUPS, NULL, 24, NULL,  0 }

#define PREGRP0	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  0, NULL,  0 }
#define PREGRP1	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  1, NULL,  0 }
#define PREGRP2	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  2, NULL,  0 }
#define PREGRP3	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  3, NULL,  0 }
#define PREGRP4	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  4, NULL,  0 }
#define PREGRP5	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  5, NULL,  0 }
#define PREGRP6	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  6, NULL,  0 }
#define PREGRP7	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  7, NULL,  0 }
#define PREGRP8	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  8, NULL,  0 }
#define PREGRP9	  { NULL, NULL, USE_PREFIX_TABLE, NULL,  9, NULL,  0 }
#define PREGRP10  { NULL, NULL, USE_PREFIX_TABLE, NULL, 10, NULL,  0 }
#define PREGRP11  { NULL, NULL, USE_PREFIX_TABLE, NULL, 11, NULL,  0 }
#define PREGRP12  { NULL, NULL, USE_PREFIX_TABLE, NULL, 12, NULL,  0 }
#define PREGRP13  { NULL, NULL, USE_PREFIX_TABLE, NULL, 13, NULL,  0 }
#define PREGRP14  { NULL, NULL, USE_PREFIX_TABLE, NULL, 14, NULL,  0 }
#define PREGRP15  { NULL, NULL, USE_PREFIX_TABLE, NULL, 15, NULL,  0 }
#define PREGRP16  { NULL, NULL, USE_PREFIX_TABLE, NULL, 16, NULL,  0 }
#define PREGRP17  { NULL, NULL, USE_PREFIX_TABLE, NULL, 17, NULL,  0 }
#define PREGRP18  { NULL, NULL, USE_PREFIX_TABLE, NULL, 18, NULL,  0 }
#define PREGRP19  { NULL, NULL, USE_PREFIX_TABLE, NULL, 19, NULL,  0 }
#define PREGRP20  { NULL, NULL, USE_PREFIX_TABLE, NULL, 20, NULL,  0 }
#define PREGRP21  { NULL, NULL, USE_PREFIX_TABLE, NULL, 21, NULL,  0 }
#define PREGRP22  { NULL, NULL, USE_PREFIX_TABLE, NULL, 22, NULL,  0 }
#define PREGRP23  { NULL, NULL, USE_PREFIX_TABLE, NULL, 23, NULL,  0 }
#define PREGRP24  { NULL, NULL, USE_PREFIX_TABLE, NULL, 24, NULL,  0 }
#define PREGRP25  { NULL, NULL, USE_PREFIX_TABLE, NULL, 25, NULL,  0 }
#define PREGRP26  { NULL, NULL, USE_PREFIX_TABLE, NULL, 26, NULL,  0 }
#define PREGRP27  { NULL, NULL, USE_PREFIX_TABLE, NULL, 27, NULL,  0 }
#define PREGRP28  { NULL, NULL, USE_PREFIX_TABLE, NULL, 28, NULL,  0 }
#define PREGRP29  { NULL, NULL, USE_PREFIX_TABLE, NULL, 29, NULL,  0 }
#define PREGRP30  { NULL, NULL, USE_PREFIX_TABLE, NULL, 30, NULL,  0 }
#define PREGRP31  { NULL, NULL, USE_PREFIX_TABLE, NULL, 31, NULL,  0 }
#define PREGRP32  { NULL, NULL, USE_PREFIX_TABLE, NULL, 32, NULL,  0 }

#define X86_64_0  { NULL, NULL, X86_64_SPECIAL, NULL,  0, NULL, 0 }

struct dis386 {
	const char *name;
	void (*op1)(int bytemode, int sizeflag, dbg_regs *);
	int bytemode1;
	void (*op2)(int bytemode, int sizeflag, dbg_regs *);
	int bytemode2;
	void (*op3)(int bytemode, int sizeflag, dbg_regs *);
	int bytemode3;
};

/* Upper case letters in the instruction names here are macros.
 * 'A' => print 'b' if no register operands or suffix_always is true
 * 'B' => print 'b' if suffix_always is true
 * 'C' => print 's' or 'l' ('w' or 'd' in Intel mode) depending on operand
 * .	  size prefix
 * 'E' => print 'e' if 32-bit form of jcxz
 * 'F' => print 'w' or 'l' depending on address size prefix (loop insns)
 * 'H' => print ", pt" or ", pn" branch hint
 * 'I' => honor following macro letter even in Intel mode (implemented only
 * .	  for some of the macro letters)
 * 'J' => print 'l'
 * 'L' => print 'l' if suffix_always is true
 * 'N' => print 'n' if instruction has no wait "prefix"
 * 'O' => print 'd', or 'o'
 * 'P' => print 'w', 'l' or 'q' if instruction has an operand size prefix,
 * .	  or suffix_always is true.  print 'q' if rex prefix is present.
 * 'Q' => print 'w', 'l' or 'q' if no register operands or suffix_always
 * .	  is true
 * 'R' => print 'w', 'l' or 'q' ("wd" or "dq" in intel mode)
 * 'S' => print 'w', 'l' or 'q' if suffix_always is true
 * 'T' => print 'q' in 64bit mode and behave as 'P' otherwise
 * 'U' => print 'q' in 64bit mode and behave as 'Q' otherwise
 * 'W' => print 'b' or 'w' ("w" or "de" in intel mode)
 * 'X' => print 's', 'd' depending on data16 prefix (for XMM)
 * 'Y' => 'q' if instruction has an REX 64bit overwrite prefix
 * Many of the above letters print nothing in Intel mode.  See "putop"
 * for the details.
 *
 * Braces '{' and '}', and vertical bars '|', indicate alternative
 * mnemonic strings for AT&T, Intel, X86_64 AT&T, and X86_64 Intel
 * modes.  In cases where there are only two alternatives, the X86_64
 * instruction is reserved, and "(bad)" is printed.
 */

static const struct dis386 dis386[] = {
	/* 00 */
	{ "addB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "addS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "addB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "addS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "addB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "addS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "push{T|}",	OP_REG, es_reg, NULL, 0, NULL, 0 },
	{ "pop{T|}",	OP_REG, es_reg, NULL, 0, NULL, 0 },
	/* 08 */
	{ "orB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "orS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "orB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "orS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "orB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "orS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "push{T|}",	OP_REG, cs_reg, NULL, 0, NULL, 0 },
	/* 0x0f extended opcode escape */
	{ "(bad)",		NULL, 0, NULL, 0, NULL, 0 },
	/* 10 */
	{ "adcB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "adcS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "adcB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "adcS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "adcB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "adcS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "push{T|}",	OP_REG, ss_reg, NULL, 0, NULL, 0 },
	{ "popT|}",	OP_REG, ss_reg, NULL, 0, NULL, 0 },
	/* 18 */
	{ "sbbB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "sbbS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "sbbB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "sbbS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "sbbB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "sbbS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "push{T|}",	OP_REG, ds_reg, NULL, 0, NULL, 0 },
	{ "pop{T|}",	OP_REG, ds_reg, NULL, 0, NULL, 0 },
	/* 20 */
	{ "andB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "andS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "andB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "andS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "andB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "andS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },	/* SEG ES prefix */
	{ "daa{|}",	NULL, 0, NULL, 0, NULL, 0 },
	/* 28 */
	{ "subB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "subS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "subB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "subS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "subB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "subS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },	/* SEG CS prefix */
	{ "das{|}",	NULL, 0, NULL, 0, NULL, 0 },
	/* 30 */
	{ "xorB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "xorS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "xorB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "xorS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "xorB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "xorS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },	/* SEG SS prefix */
	{ "aaa{|}",	NULL, 0, NULL, 0, NULL, 0 },
	/* 38 */
	{ "cmpB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "cmpS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "cmpB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "cmpS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmpB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "cmpS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },	/* SEG DS prefix */
	{ "aas{|}",	NULL, 0, NULL, 0, NULL, 0 },
	/* 40 */
	{ "inc{S|}",	OP_REG, eax_reg, NULL, 0, NULL, 0 },
	{ "inc{S|}",	OP_REG, ecx_reg, NULL, 0, NULL, 0 },
	{ "inc{S|}",	OP_REG, edx_reg, NULL, 0, NULL, 0 },
	{ "inc{S|}",	OP_REG, ebx_reg, NULL, 0, NULL, 0 },
	{ "inc{S|}",	OP_REG, esp_reg, NULL, 0, NULL, 0 },
	{ "inc{S|}",	OP_REG, ebp_reg, NULL, 0, NULL, 0 },
	{ "inc{S|}",	OP_REG, esi_reg, NULL, 0, NULL, 0 },
	{ "inc{S|}",	OP_REG, edi_reg, NULL, 0, NULL, 0 },
	/* 48 */
	{ "dec{S|}",	OP_REG, eax_reg, NULL, 0, NULL, 0 },
	{ "dec{S|}",	OP_REG, ecx_reg, NULL, 0, NULL, 0 },
	{ "dec{S|}",	OP_REG, edx_reg, NULL, 0, NULL, 0 },
	{ "dec{S|}",	OP_REG, ebx_reg, NULL, 0, NULL, 0 },
	{ "dec{S|}",	OP_REG, esp_reg, NULL, 0, NULL, 0 },
	{ "dec{S|}",	OP_REG, ebp_reg, NULL, 0, NULL, 0 },
	{ "dec{S|}",	OP_REG, esi_reg, NULL, 0, NULL, 0 },
	{ "dec{S|}",	OP_REG, edi_reg, NULL, 0, NULL, 0 },
	/* 50 */
	{ "pushS",	OP_REG, rax_reg, NULL, 0, NULL, 0 },
	{ "pushS",	OP_REG, rcx_reg, NULL, 0, NULL, 0 },
	{ "pushS",	OP_REG, rdx_reg, NULL, 0, NULL, 0 },
	{ "pushS",	OP_REG, rbx_reg, NULL, 0, NULL, 0 },
	{ "pushS",	OP_REG, rsp_reg, NULL, 0, NULL, 0 },
	{ "pushS",	OP_REG, rbp_reg, NULL, 0, NULL, 0 },
	{ "pushS",	OP_REG, rsi_reg, NULL, 0, NULL, 0 },
	{ "pushS",	OP_REG, rdi_reg, NULL, 0, NULL, 0 },
	/* 58 */
	{ "popS",	OP_REG, rax_reg, NULL, 0, NULL, 0 },
	{ "popS",	OP_REG, rcx_reg, NULL, 0, NULL, 0 },
	{ "popS",	OP_REG, rdx_reg, NULL, 0, NULL, 0 },
	{ "popS",	OP_REG, rbx_reg, NULL, 0, NULL, 0 },
	{ "popS",	OP_REG, rsp_reg, NULL, 0, NULL, 0 },
	{ "popS",	OP_REG, rbp_reg, NULL, 0, NULL, 0 },
	{ "popS",	OP_REG, rsi_reg, NULL, 0, NULL, 0 },
	{ "popS",	OP_REG, rdi_reg, NULL, 0, NULL, 0 },
	/* 60 */
	{ "pusha{P|}",	NULL, 0, NULL, 0, NULL, 0 },
	{ "popa{P|}",		NULL, 0, NULL, 0, NULL, 0 },
	{ "bound{S|}",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	X86_64_0,
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 }, /* seg fs */
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 }, /* seg gs */
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 }, /* op size prefix */
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 }, /* adr size prefix */
	/* 68 */
	{ "pushT",	OP_I, q_mode, NULL, 0, NULL, 0 },
	{ "imulS",	OP_G, v_mode, OP_E, v_mode, OP_I, v_mode },
	{ "pushT",	OP_SI, b_mode, NULL, 0, NULL, 0 },
	{ "imulS",	OP_G, v_mode, OP_E, v_mode, OP_SI, b_mode },
	{ "ins{b||b|}",	OP_ESREG, edi_reg, OP_IMREG, indir_dx_reg, NULL, 0 },
	{ "ins{R||R|}",	OP_ESREG, edi_reg, OP_IMREG, indir_dx_reg, NULL, 0 },
	{ "outs{b||b|}", OP_IMREG, indir_dx_reg, OP_DSREG, esi_reg, NULL, 0 },
	{ "outs{R||R|}", OP_IMREG, indir_dx_reg, OP_DSREG, esi_reg, NULL, 0 },
	/* 70 */
	{ "joH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jnoH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jbH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jaeH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jeH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jneH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jbeH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jaH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	/* 78 */
	{ "jsH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jnsH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jpH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jnpH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jlH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jgeH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jleH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jgH",	OP_J, b_mode, NULL, 0, NULL, cond_jump_mode },
	/* 80 */
	GRP1B,
	GRP1S,
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	GRP1SS,
	{ "testB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "testS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "xchgB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "xchgS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	/* 88 */
	{ "movB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "movS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "movB",	OP_G, b_mode, OP_E, b_mode, NULL, 0 },
	{ "movS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "movQ",	SEG_FIXUP, v_mode, OP_SEG, w_mode, NULL, 0 },
	{ "leaS",	OP_G, v_mode, OP_M, 0, NULL, 0 },
	{ "movQ",	OP_SEG, w_mode, SEG_FIXUP, v_mode, NULL, 0 },
	{ "popU",	OP_E, v_mode, NULL, 0, NULL, 0 },
	/* 90 */
	{ "nop",	NOP_FIXUP, 0, NULL, 0, NULL, 0 },
	{ "xchgS",	OP_REG, ecx_reg, OP_IMREG, eax_reg, NULL, 0 },
	{ "xchgS",	OP_REG, edx_reg, OP_IMREG, eax_reg, NULL, 0 },
	{ "xchgS",	OP_REG, ebx_reg, OP_IMREG, eax_reg, NULL, 0 },
	{ "xchgS",	OP_REG, esp_reg, OP_IMREG, eax_reg, NULL, 0 },
	{ "xchgS",	OP_REG, ebp_reg, OP_IMREG, eax_reg, NULL, 0 },
	{ "xchgS",	OP_REG, esi_reg, OP_IMREG, eax_reg, NULL, 0 },
	{ "xchgS",	OP_REG, edi_reg, OP_IMREG, eax_reg, NULL, 0 },
	/* 98 */
	{ "cW{tR||tR|}", NULL, 0, NULL, 0, NULL, 0 },
	{ "cR{tO||tO|}", NULL, 0, NULL, 0, NULL, 0 },
	{ "Jcall{T|}",	OP_DIR, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },	/* fwait */
	{ "pushfT",	NULL, 0, NULL, 0, NULL, 0 },
	{ "popfT",	NULL, 0, NULL, 0, NULL, 0 },
	{ "sahf{|}",	NULL, 0, NULL, 0, NULL, 0 },
	{ "lahf{|}",	NULL, 0, NULL, 0, NULL, 0 },
	/* a0 */
	{ "movB",	OP_IMREG, al_reg, OP_OFF64, b_mode, NULL, 0 },
	{ "movS",	OP_IMREG, eax_reg, OP_OFF64, v_mode, NULL, 0 },
	{ "movB",	OP_OFF64, b_mode, OP_IMREG, al_reg, NULL, 0 },
	{ "movS",	OP_OFF64, v_mode, OP_IMREG, eax_reg, NULL, 0 },
	{ "movs{b||b|}", OP_ESREG, edi_reg, OP_DSREG, esi_reg, NULL, 0 },
	{ "movs{R||R|}", OP_ESREG, edi_reg, OP_DSREG, esi_reg, NULL, 0 },
	{ "cmps{b||b|}", OP_DSREG, esi_reg, OP_ESREG, edi_reg, NULL, 0 },
	{ "cmps{R||R|}", OP_DSREG, esi_reg, OP_ESREG, edi_reg, NULL, 0 },
	/* a8 */
	{ "testB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "testS",	OP_IMREG, eax_reg, OP_I, v_mode, NULL, 0 },
	{ "stosB",	OP_ESREG, edi_reg, OP_IMREG, al_reg, NULL, 0 },
	{ "stosS",	OP_ESREG, edi_reg, OP_IMREG, eax_reg, NULL, 0 },
	{ "lodsB",	OP_IMREG, al_reg, OP_DSREG, esi_reg, NULL, 0 },
	{ "lodsS",	OP_IMREG, eax_reg, OP_DSREG, esi_reg, NULL, 0 },
	{ "scasB",	OP_IMREG, al_reg, OP_ESREG, edi_reg, NULL, 0 },
	{ "scasS",	OP_IMREG, eax_reg, OP_ESREG, edi_reg, NULL, 0 },
	/* b0 */
	{ "movB",	OP_REG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "movB",	OP_REG, cl_reg, OP_I, b_mode, NULL, 0 },
	{ "movB",	OP_REG, dl_reg, OP_I, b_mode, NULL, 0 },
	{ "movB",	OP_REG, bl_reg, OP_I, b_mode, NULL, 0 },
	{ "movB",	OP_REG, ah_reg, OP_I, b_mode, NULL, 0 },
	{ "movB",	OP_REG, ch_reg, OP_I, b_mode, NULL, 0 },
	{ "movB",	OP_REG, dh_reg, OP_I, b_mode, NULL, 0 },
	{ "movB",	OP_REG, bh_reg, OP_I, b_mode, NULL, 0 },
	/* b8 */
	{ "movS",	OP_REG, eax_reg, OP_I64, v_mode, NULL, 0 },
	{ "movS",	OP_REG, ecx_reg, OP_I64, v_mode, NULL, 0 },
	{ "movS",	OP_REG, edx_reg, OP_I64, v_mode, NULL, 0 },
	{ "movS",	OP_REG, ebx_reg, OP_I64, v_mode, NULL, 0 },
	{ "movS",	OP_REG, esp_reg, OP_I64, v_mode, NULL, 0 },
	{ "movS",	OP_REG, ebp_reg, OP_I64, v_mode, NULL, 0 },
	{ "movS",	OP_REG, esi_reg, OP_I64, v_mode, NULL, 0 },
	{ "movS",	OP_REG, edi_reg, OP_I64, v_mode, NULL, 0 },
	/* c0 */
	GRP2B,
	GRP2S,
	{ "retT",	OP_I, w_mode, NULL, 0, NULL, 0 },
	{ "retT",	NULL, 0, NULL, 0, NULL, 0 },
	{ "les{S|}",	OP_G, v_mode, OP_M, f_mode, NULL, 0 },
	{ "ldsS",	OP_G, v_mode, OP_M, f_mode, NULL, 0 },
	{ "movA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
	{ "movQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
	/* c8 */
	{ "enterT",	OP_I, w_mode, OP_I, b_mode, NULL, 0 },
	{ "leaveT",	NULL, 0, NULL, 0, NULL, 0 },
	{ "lretP",	OP_I, w_mode, NULL, 0, NULL, 0 },
	{ "lretP",	NULL, 0, NULL, 0, NULL, 0 },
	{ "int3",	NULL, 0, NULL, 0, NULL, 0 },
	{ "int",	OP_I, b_mode, NULL, 0, NULL, 0 },
	{ "into{|}",	NULL, 0, NULL, 0, NULL, 0 },
	{ "iretP",	NULL, 0, NULL, 0, NULL, 0 },
	/* d0 */
	GRP2B_ONE,
	GRP2S_ONE,
	GRP2B_CL,
	GRP2S_CL,
	{ "aam{|}",	OP_SI, b_mode, NULL, 0, NULL, 0 },
	{ "aad{|}",	OP_SI, b_mode, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "xlat",	OP_DSREG, ebx_reg, NULL, 0, NULL, 0 },
	/* d8 */
	FLOAT,
	FLOAT,
	FLOAT,
	FLOAT,
	FLOAT,
	FLOAT,
	FLOAT,
	FLOAT,
	/* e0 */
	{ "loopneFH",	OP_J, b_mode, NULL, 0, NULL, loop_jcxz_mode },
	{ "loopeFH",	OP_J, b_mode, NULL, 0, NULL, loop_jcxz_mode },
	{ "loopFH",	OP_J, b_mode, NULL, 0, NULL, loop_jcxz_mode },
	{ "jEcxzH",	OP_J, b_mode, NULL, 0, NULL, loop_jcxz_mode },
	{ "inB",	OP_IMREG, al_reg, OP_I, b_mode, NULL, 0 },
	{ "inS",	OP_IMREG, eax_reg, OP_I, b_mode, NULL, 0 },
	{ "outB",	OP_I, b_mode, OP_IMREG, al_reg, NULL, 0 },
	{ "outS",	OP_I, b_mode, OP_IMREG, eax_reg, NULL, 0 },
	/* e8 */
	{ "callT",	OP_J, v_mode, NULL, 0, NULL, 0 },
	{ "jmpT",	OP_J, v_mode, NULL, 0, NULL, 0 },
	{ "Jjmp{T|}",	OP_DIR, 0, NULL, 0, NULL, 0 },
	{ "jmp",	OP_J, b_mode, NULL, 0, NULL, 0 },
	{ "inB",	OP_IMREG, al_reg, OP_IMREG, indir_dx_reg, NULL, 0 },
	{ "inS",	OP_IMREG, eax_reg, OP_IMREG, indir_dx_reg, NULL, 0 },
	{ "outB",	OP_IMREG, indir_dx_reg, OP_IMREG, al_reg, NULL, 0 },
	{ "outS",	OP_IMREG, indir_dx_reg, OP_IMREG, eax_reg, NULL, 0 },
	/* f0 */
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },	/* lock prefix */
	{ "icebp",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },	/* repne */
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },	/* repz */
	{ "hlt",	NULL, 0, NULL, 0, NULL, 0 },
	{ "cmc",	NULL, 0, NULL, 0, NULL, 0 },
	GRP3B,
	GRP3S,
	/* f8 */
	{ "clc",	NULL, 0, NULL, 0, NULL, 0 },
	{ "stc",	NULL, 0, NULL, 0, NULL, 0 },
	{ "cli",	NULL, 0, NULL, 0, NULL, 0 },
	{ "sti",	NULL, 0, NULL, 0, NULL, 0 },
	{ "cld",	NULL, 0, NULL, 0, NULL, 0 },
	{ "std",	NULL, 0, NULL, 0, NULL, 0 },
	GRP4,
	GRP5,
};

static const struct dis386 dis386_twobyte[] = {
	/* 00 */
	GRP6,
	GRP7,
	{ "larS",	OP_G, v_mode, OP_E, w_mode, NULL, 0 },
	{ "lslS",	OP_G, v_mode, OP_E, w_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "syscall",	NULL, 0, NULL, 0, NULL, 0 },
	{ "clts",	NULL, 0, NULL, 0, NULL, 0 },
	{ "sysretP",	NULL, 0, NULL, 0, NULL, 0 },
	/* 08 */
	{ "invd",	NULL, 0, NULL, 0, NULL, 0 },
	{ "wbinvd",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "ud2a",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	GRPAMD,
	{ "femms",	NULL, 0, NULL, 0, NULL, 0 },
	{ "",		OP_MMX, 0, OP_EM, v_mode, OP_3DNOWSUFFIX, 0 },
	/* See OP_3DNOWSUFFIX. */
	/* 10 */
	PREGRP8,
	PREGRP9,
	PREGRP30,
	{ "movlpX",	OP_EX, v_mode, OP_XMM, 0, SIMD_FIXUP, 'h' },
	{ "unpcklpX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	{ "unpckhpX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	PREGRP31,
	{ "movhpX",	OP_EX, v_mode, OP_XMM, 0, SIMD_FIXUP, 'l' },
	/* 18 */
	GRP14,
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "nopQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
	/* 20 */
	{ "movL",	OP_RD, m_mode, OP_C, m_mode, NULL, 0 },
	{ "movL",	OP_RD, m_mode, OP_D, m_mode, NULL, 0 },
	{ "movL",	OP_C, m_mode, OP_RD, m_mode, NULL, 0 },
	{ "movL",	OP_D, m_mode, OP_RD, m_mode, NULL, 0 },
	{ "movL",	OP_RD, d_mode, OP_T, d_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "movL",	OP_T, d_mode, OP_RD, d_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	/* 28 */
	{ "movapX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	{ "movapX",	OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
	PREGRP2,
	{ "movntpX",	OP_E, v_mode, OP_XMM, 0, NULL, 0 },
	PREGRP4,
	PREGRP3,
	{ "ucomisX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	{ "comisX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	/* 30 */
	{ "wrmsr",	NULL, 0, NULL, 0, NULL, 0 },
	{ "rdtsc",	NULL, 0, NULL, 0, NULL, 0 },
	{ "rdmsr",	NULL, 0, NULL, 0, NULL, 0 },
	{ "rdpmc",	NULL, 0, NULL, 0, NULL, 0 },
	{ "sysenter",	NULL, 0, NULL, 0, NULL, 0 },
	{ "sysexit",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	/* 38 */
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	/* 40 */
	{ "cmovo",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovno",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovb",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovae",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmove",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovne",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovbe",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmova",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	/* 48 */
	{ "cmovs",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovns",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovp",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovnp",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovl",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovge",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovle",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "cmovg",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	/* 50 */
	{ "movmskpX",	OP_G, dq_mode, OP_XS, v_mode, NULL, 0 },
	PREGRP13,
	PREGRP12,
	PREGRP11,
	{ "andpX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	{ "andnpX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	{ "orpX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	{ "xorpX",	OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	/* 58 */
	PREGRP0,
	PREGRP10,
	PREGRP17,
	PREGRP16,
	PREGRP14,
	PREGRP7,
	PREGRP5,
	PREGRP6,
	/* 60 */
	{ "punpcklbw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "punpcklwd",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "punpckldq",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "packsswb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pcmpgtb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pcmpgtw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pcmpgtd",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "packuswb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	/* 68 */
	{ "punpckhbw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "punpckhwd",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "punpckhdq",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "packssdw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	PREGRP26,
	PREGRP24,
	{ "movd",	OP_MMX, 0, OP_E, dq_mode, NULL, 0 },
	PREGRP19,
	/* 70 */
	PREGRP22,
	GRP10,
	GRP11,
	GRP12,
	{ "pcmpeqb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pcmpeqw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pcmpeqd",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "emms",	NULL, 0, NULL, 0, NULL, 0 },
	/* 78 */
	{ "vmread",	OP_E, m_mode, OP_G, m_mode, NULL, 0 },
	{ "vmwrite",	OP_G, m_mode, OP_E, m_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	PREGRP28,
	PREGRP29,
	PREGRP23,
	PREGRP20,
	/* 80 */
	{ "joH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jnoH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jbH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jaeH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jeH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jneH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jbeH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jaH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	/* 88 */
	{ "jsH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jnsH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jpH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jnpH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jlH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jgeH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jleH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	{ "jgH",	OP_J, v_mode, NULL, 0, NULL, cond_jump_mode },
	/* 90 */
	{ "seto",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setno",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setb",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setae",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "sete",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setne",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setbe",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "seta",	OP_E, b_mode, NULL, 0, NULL, 0 },
	/* 98 */
	{ "sets",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setns",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setp",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setnp",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setl",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setge",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setle",	OP_E, b_mode, NULL, 0, NULL, 0 },
	{ "setg",	OP_E, b_mode, NULL, 0, NULL, 0 },
	/* a0 */
	{ "pushT",	OP_REG, fs_reg, NULL, 0, NULL, 0 },
	{ "popT",	OP_REG, fs_reg, NULL, 0, NULL, 0 },
	{ "cpuid",	NULL, 0, NULL, 0, NULL, 0 },
	{ "btS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "shldS",	OP_E, v_mode, OP_G, v_mode, OP_I, b_mode },
	{ "shldS",	OP_E, v_mode, OP_G, v_mode, OP_IMREG, cl_reg },
	GRPPADLCK2,
	GRPPADLCK1,
	/* a8 */
	{ "pushT",	OP_REG, gs_reg, NULL, 0, NULL, 0 },
	{ "popT",	OP_REG, gs_reg, NULL, 0, NULL, 0 },
	{ "rsm",	NULL, 0, NULL, 0, NULL, 0 },
	{ "btsS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "shrdS",	OP_E, v_mode, OP_G, v_mode, OP_I, b_mode },
	{ "shrdS",	OP_E, v_mode, OP_G, v_mode, OP_IMREG, cl_reg },
	GRP13,
	{ "imulS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	/* b0 */
	{ "cmpxchgB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "cmpxchgS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "lssS",	OP_G, v_mode, OP_M, f_mode, NULL, 0 },
	{ "btrS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "lfsS",	OP_G, v_mode, OP_M, f_mode, NULL, 0 },
	{ "lgsS",	OP_G, v_mode, OP_M, f_mode, NULL, 0 },
	{ "movz{bR|x|bR|x}", OP_G, v_mode, OP_E, b_mode, NULL, 0 },
	{ "movz{wR|x|wR|x}", OP_G, v_mode, OP_E, w_mode, NULL, 0 },
	/* yes, there really is movzww ! */
	/* b8 */
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	{ "ud2b",	NULL, 0, NULL, 0, NULL, 0 },
	GRP8,
	{ "btcS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "bsfS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "bsrS",	OP_G, v_mode, OP_E, v_mode, NULL, 0 },
	{ "movs{bR|x|bR|x}", OP_G, v_mode, OP_E, b_mode, NULL, 0 },
	{ "movs{wR|x|wR|x}", OP_G, v_mode, OP_E, w_mode, NULL, 0 },
	/* yes, there really is movsww ! */
	/* c0 */
	{ "xaddB",	OP_E, b_mode, OP_G, b_mode, NULL, 0 },
	{ "xaddS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	PREGRP1,
	{ "movntiS",	OP_E, v_mode, OP_G, v_mode, NULL, 0 },
	{ "pinsrw",	OP_MMX, 0, OP_E, dqw_mode, OP_I, b_mode },
	{ "pextrw",	OP_G, dq_mode, OP_MS, v_mode, OP_I, b_mode },
	{ "shufpX",	OP_XMM, 0, OP_EX, v_mode, OP_I, b_mode },
	GRP9,
	/* c8 */
	{ "bswap",	OP_REG, eax_reg, NULL, 0, NULL, 0 },
	{ "bswap",	OP_REG, ecx_reg, NULL, 0, NULL, 0 },
	{ "bswap",	OP_REG, edx_reg, NULL, 0, NULL, 0 },
	{ "bswap",	OP_REG, ebx_reg, NULL, 0, NULL, 0 },
	{ "bswap",	OP_REG, esp_reg, NULL, 0, NULL, 0 },
	{ "bswap",	OP_REG, ebp_reg, NULL, 0, NULL, 0 },
	{ "bswap",	OP_REG, esi_reg, NULL, 0, NULL, 0 },
	{ "bswap",	OP_REG, edi_reg, NULL, 0, NULL, 0 },
	/* d0 */
	PREGRP27,
	{ "psrlw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psrld",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psrlq",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "paddq",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pmullw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	PREGRP21,
	{ "pmovmskb",	OP_G, dq_mode, OP_MS, v_mode, NULL, 0 },
	/* d8 */
	{ "psubusb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psubusw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pminub",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pand",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "paddusb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "paddusw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pmaxub",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pandn",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	/* e0 */
	{ "pavgb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psraw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psrad",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pavgw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pmulhuw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pmulhw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	PREGRP15,
	PREGRP25,
	/* e8 */
	{ "psubsb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psubsw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pminsw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "por",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "paddsb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "paddsw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pmaxsw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pxor",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	/* f0 */
	PREGRP32,
	{ "psllw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pslld",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psllq",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pmuludq",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "pmaddwd",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psadbw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	PREGRP18,
	/* f8 */
	{ "psubb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psubw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psubd",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "psubq",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "paddb",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "paddw",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "paddd",	OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
	{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 }
};

static const unsigned char onebyte_has_modrm[256] = {
	/*	   0 1 2 3 4 5 6 7 8 9 a b c d e f	  */
	/*	   -------------------------------	  */
	/* 00 */ 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, /* 00 */
	/* 10 */ 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, /* 10 */
	/* 20 */ 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, /* 20 */
	/* 30 */ 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, /* 30 */
	/* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 40 */
	/* 50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 50 */
	/* 60 */ 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, /* 60 */
	/* 70 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 70 */
	/* 80 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 80 */
	/* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 90 */
	/* a0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* a0 */
	/* b0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* b0 */
	/* c0 */ 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, /* c0 */
	/* d0 */ 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, /* d0 */
	/* e0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* e0 */
	/* f0 */ 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1  /* f0 */
		/*	   -------------------------------	  */
		/*	   0 1 2 3 4 5 6 7 8 9 a b c d e f	  */
};

static const unsigned char twobyte_has_modrm[256] = {
	/*	   0 1 2 3 4 5 6 7 8 9 a b c d e f	  */
	/*	   -------------------------------	  */
	/* 00 */ 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, /* 0f */
	/* 10 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, /* 1f */
	/* 20 */ 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, /* 2f */
	/* 30 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 3f */
	/* 40 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 4f */
	/* 50 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 5f */
	/* 60 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 6f */
	/* 70 */ 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, /* 7f */
	/* 80 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 8f */
	/* 90 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 9f */
	/* a0 */ 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, /* af */
	/* b0 */ 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, /* bf */
	/* c0 */ 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, /* cf */
	/* d0 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* df */
	/* e0 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* ef */
	/* f0 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0  /* ff */
		/*	   -------------------------------	  */
		/*	   0 1 2 3 4 5 6 7 8 9 a b c d e f	  */
};

static const unsigned char twobyte_uses_SSE_prefix[256] = {
	/*	   0 1 2 3 4 5 6 7 8 9 a b c d e f	  */
	/*	   -------------------------------	  */
	/* 00 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0f */
	/* 10 */ 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 1f */
	/* 20 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, /* 2f */
	/* 30 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 3f */
	/* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 4f */
	/* 50 */ 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, /* 5f */
	/* 60 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, /* 6f */
	/* 70 */ 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* 7f */
	/* 80 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 8f */
	/* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 9f */
	/* a0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* af */
	/* b0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* bf */
	/* c0 */ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* cf */
	/* d0 */ 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* df */
	/* e0 */ 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* ef */
	/* f0 */ 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0  /* ff */
		/*	   -------------------------------	  */
		/*	   0 1 2 3 4 5 6 7 8 9 a b c d e f	  */
};

static char obuf[512];
static char *obufp;
static char scratchbuf[512];
static unsigned char *start_codep;
static unsigned char *insn_codep;
static unsigned char *codep;
static struct disassemble_info *the_info;
static int mod;
static int rm;
static int reg;
static unsigned char need_modrm;

/* If we are accessing mod/rm/reg without need_modrm set, then the
 * values are stale.  Hitting this abort likely indicates that you
 * need to update onebyte_has_modrm or twobyte_has_modrm.
 */
#define MODRM_CHECK			\
do {					\
	if (!need_modrm)		\
		abort();		\
} while (0)

const char **names64;
const char **names32;
const char **names16;
const char **names8;
const char **names8rex;
const char **names_seg;
const char **index16;

const char *intel_names64[] = {
	"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

const char *intel_names32[] = {
	"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
	"r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
};

const char *intel_names16[] = {
	"ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
	"r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"
};

const char *intel_names8[] = {
	"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh",
};

const char *intel_names8rex[] = {
	"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
	"r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
};

const char *intel_names_seg[] = {
	"es", "cs", "ss", "ds", "fs", "gs", "?", "?",
};

const char *intel_index16[] = {
	"bx+si", "bx+di", "bp+si", "bp+di", "si", "di", "bp", "bx"
};

const char *att_names64[] = {
	"%rax", "%rcx", "%rdx", "%rbx", "%rsp", "%rbp", "%rsi", "%rdi",
	"%r8", "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15"
};

const char *att_names32[] = {
	"%eax", "%ecx", "%edx", "%ebx", "%esp", "%ebp", "%esi", "%edi",
	"%r8d", "%r9d", "%r10d", "%r11d", "%r12d", "%r13d", "%r14d", "%r15d"
};

const char *att_names16[] = {
	"%ax", "%cx", "%dx", "%bx", "%sp", "%bp", "%si", "%di",
	"%r8w", "%r9w", "%r10w", "%r11w", "%r12w", "%r13w", "%r14w", "%r15w"
};

const char *att_names8[] = {
	"%al", "%cl", "%dl", "%bl", "%ah", "%ch", "%dh", "%bh",
};

const char *att_names8rex[] = {
	"%al", "%cl", "%dl", "%bl", "%spl", "%bpl", "%sil", "%dil",
	"%r8b", "%r9b", "%r10b", "%r11b", "%r12b", "%r13b", "%r14b", "%r15b"
};

const char *att_names_seg[] = {
	"%es", "%cs", "%ss", "%ds", "%fs", "%gs", "%?", "%?",
};

const char *att_index16[] = {
	"%bx,%si", "%bx,%di", "%bp,%si", "%bp,%di", "%si", "%di", "%bp",
	"%bx"
};

static const struct dis386 grps[][8] = {
	/* GRP1B */
	{
		{ "addA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "orA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "adcA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "sbbA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "andA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "subA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "xorA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "cmpA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 }
	},
	/* GRP1S */
	{
		{ "addQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
		{ "orQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
		{ "adcQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
		{ "sbbQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
		{ "andQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
		{ "subQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
		{ "xorQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
		{ "cmpQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 }
	},
	/* GRP1SS */
	{
		{ "addQ",	OP_E, v_mode, OP_SI, b_mode, NULL, 0 },
		{ "orQ",	OP_E, v_mode, OP_SI, b_mode, NULL, 0 },
		{ "adcQ",	OP_E, v_mode, OP_SI, b_mode, NULL, 0 },
		{ "sbbQ",	OP_E, v_mode, OP_SI, b_mode, NULL, 0 },
		{ "andQ",	OP_E, v_mode, OP_SI, b_mode, NULL, 0 },
		{ "subQ",	OP_E, v_mode, OP_SI, b_mode, NULL, 0 },
		{ "xorQ",	OP_E, v_mode, OP_SI, b_mode, NULL, 0 },
		{ "cmpQ",	OP_E, v_mode, OP_SI, b_mode, NULL, 0 }
	},
	/* GRP2B */
	{
		{ "rolA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "rorA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "rclA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "rcrA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "shlA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "shrA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "sarA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
	},
	/* GRP2S */
	{
		{ "rolQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "rorQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "rclQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "rcrQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "shlQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "shrQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "sarQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
	},
	/* GRP2B_ONE */
	{
		{ "rolA",	OP_E, b_mode, OP_I, const_1_mode, NULL, 0 },
		{ "rorA",	OP_E, b_mode, OP_I, const_1_mode, NULL, 0 },
		{ "rclA",	OP_E, b_mode, OP_I, const_1_mode, NULL, 0 },
		{ "rcrA",	OP_E, b_mode, OP_I, const_1_mode, NULL, 0 },
		{ "shlA",	OP_E, b_mode, OP_I, const_1_mode, NULL, 0 },
		{ "shrA",	OP_E, b_mode, OP_I, const_1_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "sarA",	OP_E, b_mode, OP_I, const_1_mode, NULL, 0 },
	},
	/* GRP2S_ONE */
	{
		{ "rolQ",	OP_E, v_mode, OP_I, const_1_mode, NULL, 0 },
		{ "rorQ",	OP_E, v_mode, OP_I, const_1_mode, NULL, 0 },
		{ "rclQ",	OP_E, v_mode, OP_I, const_1_mode, NULL, 0 },
		{ "rcrQ",	OP_E, v_mode, OP_I, const_1_mode, NULL, 0 },
		{ "shlQ",	OP_E, v_mode, OP_I, const_1_mode, NULL, 0 },
		{ "shrQ",	OP_E, v_mode, OP_I, const_1_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0},
		{ "sarQ",	OP_E, v_mode, OP_I, const_1_mode, NULL, 0 },
	},
	/* GRP2B_CL */
	{
		{ "rolA",	OP_E, b_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "rorA",	OP_E, b_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "rclA",	OP_E, b_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "rcrA",	OP_E, b_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "shlA",	OP_E, b_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "shrA",	OP_E, b_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "sarA",	OP_E, b_mode, OP_IMREG, cl_reg, NULL, 0 },
	},
	/* GRP2S_CL */
	{
		{ "rolQ",	OP_E, v_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "rorQ",	OP_E, v_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "rclQ",	OP_E, v_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "rcrQ",	OP_E, v_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "shlQ",	OP_E, v_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "shrQ",	OP_E, v_mode, OP_IMREG, cl_reg, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "sarQ",	OP_E, v_mode, OP_IMREG, cl_reg, NULL, 0 }
	},
	/* GRP3B */
	{
		{ "testA",	OP_E, b_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	OP_E, b_mode, NULL, 0, NULL, 0 },
		{ "notA",	OP_E, b_mode, NULL, 0, NULL, 0 },
		{ "negA",	OP_E, b_mode, NULL, 0, NULL, 0 },
		{ "mulA",	OP_E, b_mode, NULL, 0, NULL, 0 },
		/* Don't print the implicit %al register,  */
		{ "imulA",	OP_E, b_mode, NULL, 0, NULL, 0 },
		/* to distinguish these opcodes from other */
		{ "divA",	OP_E, b_mode, NULL, 0, NULL, 0 },
		/* mul/imul opcodes.  Do the same for div  */
		{ "idivA",	OP_E, b_mode, NULL, 0, NULL, 0 }
		/* and idiv for consistency.		   */
	},
	/* GRP3S */
	{
		{ "testQ",	OP_E, v_mode, OP_I, v_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "notQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "negQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "mulQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		/* Don't print the implicit register.  */
		{ "imulQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "divQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "idivQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
	},
	/* GRP4 */
	{
		{ "incA",	OP_E, b_mode, NULL, 0, NULL, 0 },
		{ "decA",	OP_E, b_mode, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* GRP5 */
	{
		{ "incQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "decQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "callT",	OP_INDIRE, branch_v_mode, NULL, 0, NULL, 0 },
		{ "JcallT",	OP_INDIRE, f_mode, NULL, 0, NULL, 0 },
		{ "jmpT",	OP_INDIRE, branch_v_mode, NULL, 0, NULL, 0 },
		{ "JjmpT",	OP_INDIRE, f_mode, NULL, 0, NULL, 0 },
		{ "pushU",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* GRP6 */
	{
		{ "sldtQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "strQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "lldt",	OP_E, w_mode, NULL, 0, NULL, 0 },
		{ "ltr",	OP_E, w_mode, NULL, 0, NULL, 0 },
		{ "verr",	OP_E, w_mode, NULL, 0, NULL, 0 },
		{ "verw",	OP_E, w_mode, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 }
	},
	/* GRP7 */
	{
		{ "sgdtIQ", VMX_FIXUP, 0, NULL, 0, NULL, 0 },
		{ "sidtIQ", PNI_FIXUP, 0, NULL, 0, NULL, 0 },
		{ "lgdt{Q|Q||}",	 OP_M, 0, NULL, 0, NULL, 0 },
		{ "lidt{Q|Q||}",	 SVME_FIXUP, 0, NULL, 0, NULL, 0 },
		{ "smswQ",	OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "lmsw",	OP_E, w_mode, NULL, 0, NULL, 0 },
		{ "invlpg", INVLPG_FIXUP, w_mode, NULL, 0, NULL, 0 },
	},
	/* GRP8 */
	{
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "btQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "btsQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "btrQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
		{ "btcQ",	OP_E, v_mode, OP_I, b_mode, NULL, 0 },
	},
	/* GRP9 */
	{
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "cmpxchg8b", OP_E, q_mode, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "",	OP_VMX, q_mode, NULL, 0, NULL, 0 },
		/* See OP_VMX.	*/
		{ "vmptrst", OP_E, q_mode, NULL, 0, NULL, 0 },
	},
	/* GRP10 */
	{
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "psrlw",	OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "psraw",	OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "psllw",	OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* GRP11 */
	{
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "psrld",	OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "psrad",	OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "pslld",	OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* GRP12 */
	{
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "psrlq",	OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "psrldq", OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "psllq",	OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
		{ "pslldq", OP_MS, v_mode, OP_I, b_mode, NULL, 0 },
	},
	/* GRP13 */
	{
		{ "fxsave", OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "fxrstor", OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "ldmxcsr", OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "stmxcsr", OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "xsave",  OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "xrstor", OP_0FAE, 0, NULL, 0, NULL, 0 },
		{ "mfence", OP_0FAE, 0, NULL, 0, NULL, 0 },
		{ "clflush", OP_0FAE, 0, NULL, 0, NULL, 0 },
	},
	/* GRP14 */
	{
		{ "prefetchnta", OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "prefetcht0", OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "prefetcht1", OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "prefetcht2", OP_E, v_mode, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* GRPAMD */
	{
		{ "prefetch", OP_E, b_mode, NULL, 0, NULL, 0 },
		{ "prefetchw", OP_E, b_mode, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* GRPPADLCK1 */
	{
		{ "xstore-rng", OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "xcrypt-ecb", OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "xcrypt-cbc", OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "xcrypt-ctr", OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "xcrypt-cfb", OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "xcrypt-ofb", OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	OP_0F07, 0, NULL, 0, NULL, 0 },
	},
	/* GRPPADLCK2 */
	{
		{ "montmul", OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "xsha1",	 OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "xsha256", OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	 OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	 OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	 OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	 OP_0F07, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	 OP_0F07, 0, NULL, 0, NULL, 0 },
	}
};

static const struct dis386 prefix_tbl[][4] = {
	/* PREGRP0 */
	{
		{ "addps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "addss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "addpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "addsd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP1 */
	{
		{ "", OP_XMM, 0, OP_EX, v_mode, OP_SIMD_SUFFIX, 0 },
		/* See OP_SIMD_SUFFIX.	*/
		{ "", OP_XMM, 0, OP_EX, v_mode, OP_SIMD_SUFFIX, 0 },
		{ "", OP_XMM, 0, OP_EX, v_mode, OP_SIMD_SUFFIX, 0 },
		{ "", OP_XMM, 0, OP_EX, v_mode, OP_SIMD_SUFFIX, 0 },
	},
	/* PREGRP2 */
	{
		{ "cvtpi2ps", OP_XMM, 0, OP_EM, v_mode, NULL, 0 },
		{ "cvtsi2ssY", OP_XMM, 0, OP_E, v_mode, NULL, 0 },
		{ "cvtpi2pd", OP_XMM, 0, OP_EM, v_mode, NULL, 0 },
		{ "cvtsi2sdY", OP_XMM, 0, OP_E, v_mode, NULL, 0 },
	},
	/* PREGRP3 */
	{
		{ "cvtps2pi", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvtss2siY", OP_G, v_mode, OP_EX, v_mode, NULL, 0 },
		{ "cvtpd2pi", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvtsd2siY", OP_G, v_mode, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP4 */
	{
		{ "cvttps2pi", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvttss2siY", OP_G, v_mode, OP_EX, v_mode, NULL, 0 },
		{ "cvttpd2pi", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvttsd2siY", OP_G, v_mode, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP5 */
	{
		{ "divps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "divss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "divpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "divsd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP6 */
	{
		{ "maxps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "maxss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "maxpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "maxsd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP7 */
	{
		{ "minps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "minss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "minpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "minsd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP8 */
	{
		{ "movups", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "movss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "movupd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "movsd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP9 */
	{
		{ "movups", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
		{ "movss", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
		{ "movupd", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
		{ "movsd", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
	},
	/* PREGRP10 */
	{
		{ "mulps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "mulss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "mulpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "mulsd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP11 */
	{
		{ "rcpps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "rcpss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP12 */
	{
		{ "rsqrtps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "rsqrtss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP13 */
	{
		{ "sqrtps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "sqrtss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "sqrtpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "sqrtsd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP14 */
	{
		{ "subps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "subss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "subpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "subsd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP15 */
	{
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvtdq2pd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvttpd2dq", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvtpd2dq", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP16 */
	{
		{ "cvtdq2ps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvttps2dq", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvtps2dq", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP17 */
	{
		{ "cvtps2pd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvtss2sd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvtpd2ps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "cvtsd2ss", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP18 */
	{
		{ "maskmovq", OP_MMX, 0, OP_MS, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "maskmovdqu", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP19 */
	{
		{ "movq", OP_MMX, 0, OP_EM, v_mode, NULL, 0 },
		{ "movdqu", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "movdqa", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP20 */
	{
		{ "movq", OP_EM, v_mode, OP_MMX, 0, NULL, 0 },
		{ "movdqu", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
		{ "movdqa", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
		{ "(bad)", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
	},
	/* PREGRP21 */
	{
		{ "(bad)", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
		{ "movq2dq", OP_XMM, 0, OP_MS, v_mode, NULL, 0 },
		{ "movq", OP_EX, v_mode, OP_XMM, 0, NULL, 0 },
		{ "movdq2q", OP_MMX, 0, OP_XS, v_mode, NULL, 0 },
	},
	/* PREGRP22 */
	{
		{ "pshufw", OP_MMX, 0, OP_EM, v_mode, OP_I, b_mode },
		{ "pshufhw", OP_XMM, 0, OP_EX, v_mode, OP_I, b_mode },
		{ "pshufd", OP_XMM, 0, OP_EX, v_mode, OP_I, b_mode },
		{ "pshuflw", OP_XMM, 0, OP_EX, v_mode, OP_I, b_mode },
	},
	/* PREGRP23 */
	{
		{ "movd", OP_E, dq_mode, OP_MMX, 0, NULL, 0 },
		{ "movq", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "movd", OP_E, dq_mode, OP_XMM, 0, NULL, 0 },
		{ "(bad)", OP_E, d_mode, OP_XMM, 0, NULL, 0 },
	},
	/* PREGRP24 */
	{
		{ "(bad)", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "punpckhqdq", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP25 */
	{
		{ "movntq", OP_EM, v_mode, OP_MMX, 0, NULL, 0 },
		{ "(bad)", OP_EM, v_mode, OP_XMM, 0, NULL, 0 },
		{ "movntdq", OP_EM, v_mode, OP_XMM, 0, NULL, 0 },
		{ "(bad)", OP_EM, v_mode, OP_XMM, 0, NULL, 0 },
	},
	/* PREGRP26 */
	{
		{ "(bad)", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "punpcklqdq", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP27 */
	{
		{ "(bad)", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "addsubpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "addsubps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP28 */
	{
		{ "(bad)", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "haddpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "haddps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP29 */
	{
		{ "(bad)", OP_MMX, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "hsubpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "hsubps", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP30 */
	{
		{ "movlpX", OP_XMM, 0, OP_EX, v_mode, SIMD_FIXUP, 'h' },
		/* really only 2 operands */
		{ "movsldup", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "movlpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "movddup", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP31 */
	{
		{ "movhpX", OP_XMM, 0, OP_EX, v_mode, SIMD_FIXUP, 'l' },
		{ "movshdup", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "movhpd", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
	},
	/* PREGRP32 */
	{
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "(bad)", OP_XMM, 0, OP_EX, v_mode, NULL, 0 },
		{ "lddqu", OP_XMM, 0, OP_M, 0, NULL, 0 },
	},
};

static const struct dis386 x86_64_table[][2] = {
	{
		{ "arpl", OP_E, w_mode, OP_G, w_mode, NULL, 0 },
		{ "movs{||lq|xd}", OP_G, v_mode, OP_E, d_mode, NULL, 0 },
	},
};

#define INTERNAL_DISASSEMBLER_ERROR "<???>"

static void
ckprefix(void)
{
	int newrex;

	rex = 0;
	prefixes = 0;
	used_prefixes = 0;
	rex_used = 0;
	while (1) {
		FETCH_DATA(the_info, codep + 1);
		newrex = 0;
		switch (*codep) {
			/* REX prefixes family.	 */
		case 0x40:
		case 0x41:
		case 0x42:
		case 0x43:
		case 0x44:
		case 0x45:
		case 0x46:
		case 0x47:
		case 0x48:
		case 0x49:
		case 0x4a:
		case 0x4b:
		case 0x4c:
		case 0x4d:
		case 0x4e:
		case 0x4f:
			if (mode_64bit)
				newrex = *codep;
			else
				return;
			break;
		case 0xf3:
			prefixes |= PREFIX_REPZ;
			break;
		case 0xf2:
			prefixes |= PREFIX_REPNZ;
			break;
		case 0xf0:
			prefixes |= PREFIX_LOCK;
			break;
		case 0x2e:
			prefixes |= PREFIX_CS;
			break;
		case 0x36:
			prefixes |= PREFIX_SS;
			break;
		case 0x3e:
			prefixes |= PREFIX_DS;
			break;
		case 0x26:
			prefixes |= PREFIX_ES;
			break;
		case 0x64:
			prefixes |= PREFIX_FS;
			break;
		case 0x65:
			prefixes |= PREFIX_GS;
			break;
		case 0x66:
			prefixes |= PREFIX_DATA;
			break;
		case 0x67:
			prefixes |= PREFIX_ADDR;
			break;
		case FWAIT_OPCODE:
			/* fwait is really an instruction.
			 * If there are prefixes before
			 * the fwait, they belong to the
			 * fwait, *not* to the following
			 * instruction.
			 */
			if (prefixes) {
				prefixes |= PREFIX_FWAIT;
				codep++;
				return;
			}
			prefixes = PREFIX_FWAIT;
			break;
		default:
			return;
		}
		/* Rex is ignored when followed by another prefix.  */
		if (rex) {
			oappend(prefix_name(rex, 0));
			oappend(" ");
		}
		rex = newrex;
		codep++;
	}
}

/* Return the name of the prefix byte PREF, or NULL
 * if PREF is not a prefix byte.
 */

static const char *
prefix_name(int pref, int sizeflag)
{
	switch (pref) {
		/* REX prefixes family.  */
	case 0x40:
		return "rex";
	case 0x41:
		return "rexZ";
	case 0x42:
		return "rexY";
	case 0x43:
		return "rexYZ";
	case 0x44:
		return "rexX";
	case 0x45:
		return "rexXZ";
	case 0x46:
		return "rexXY";
	case 0x47:
		return "rexXYZ";
	case 0x48:
		return "rex64";
	case 0x49:
		return "rex64Z";
	case 0x4a:
		return "rex64Y";
	case 0x4b:
		return "rex64YZ";
	case 0x4c:
		return "rex64X";
	case 0x4d:
		return "rex64XZ";
	case 0x4e:
		return "rex64XY";
	case 0x4f:
		return "rex64XYZ";
	case 0xf3:
		return "repz";
	case 0xf2:
		return "repnz";
	case 0xf0:
		return "lock";
	case 0x2e:
		return "cs";
	case 0x36:
		return "ss";
	case 0x3e:
		return "ds";
	case 0x26:
		return "es";
	case 0x64:
		return "fs";
	case 0x65:
		return "gs";
	case 0x66:
		return (sizeflag & DFLAG) ? "data16" : "data32";
	case 0x67:
		if (mode_64bit)
			return (sizeflag & AFLAG) ? "addr32" : "addr64";
		else
			return (sizeflag & AFLAG) ? "addr16" : "addr32";
	case FWAIT_OPCODE:
		return "fwait";
	default:
		return NULL;
	}
}

static char op1out[256], op2out[256], op3out[256];
static int op_ad, op_index[3];
static int two_source_ops;
static mdb_vma op_address[3];
static mdb_vma op_riprel[3];
static mdb_vma start_pc;

/*   On the 386's of 1988, the maximum length of an instruction is 15 bytes.
 *   (see topic "Redundant prefixes" in the "Differences from 8086"
 *   section of the "Virtual 8086 Mode" chapter.)
 * 'pc' should be the address of this instruction, it will
 *   be used to print the target address if this is a relative jump or call
 * The function returns the length of this instruction in bytes.
 */

static char intel_syntax;
static char open_char;
static char close_char;
static char separator_char;
static char scale_char;

/* Here for backwards compatibility.  When gdb stops
 * using print_insn_i386_att and print_insn_i386_intel
 * these functions can disappear, and print_insn_i386
 * be merged into print_insn.
 */
int
print_insn_i386_att(mdb_vma pc, struct disassemble_info *info,
		    dbg_regs *dbgframe)
{
	intel_syntax = 0;

	return print_insn(pc, info, dbgframe);
}

int
print_insn_i386_intel(mdb_vma pc, struct disassemble_info *info,
		      dbg_regs *dbgframe)
{
	intel_syntax = 1;

	return print_insn(pc, info, dbgframe);
}

int
print_insn_i386(mdb_vma pc, struct disassemble_info *info,
		dbg_regs *dbgframe)
{
	intel_syntax = -1;

	return print_insn(pc, info, dbgframe);
}

static inline void output_jmp_address(dbg_regs *dbgframe,
				      unsigned long val,
				      char *ubuf,
				      struct disassemble_info *info)
{
	if (!strncasecmp(ubuf, "j", 1) || !strncasecmp(ubuf, "loop", 4)) {
		jmp_active = 1;
		if (intel_syntax)
			(*info->fprintf_func)(info->stream, " (0x%p) %s",
					      (void *)val,
					      (val < (unsigned long)vaddr) ?
					      "(up)" : "(down)");
		else
			(*info->fprintf_func)(info->stream, " %s",
					      (val < (unsigned long)vaddr) ?
					      "(up)" : "(down)");
	}
}

#define BFMT6	"=0x%2X%2X%2X%2X%2X%2X"
#define BFMT10	"=0x%2X%2X%2X%2X%2X%2X%2X%2X%2X%2X"
#define BFMT16	"=0x%2X%2X%2X%2X%2X%2X%2X%2X%2X%2X%2X%2X%2X%2X%2X%2X"

static inline void bfd_eval_size(int sz, int sizeflag,
				 unsigned char *deref,
				 dbg_regs *dbgframe)
{
	unsigned char *cmd = deref;
	unsigned long long value;
	unsigned long valid;
	unsigned char *r = NULL;
	unsigned char work[16];

	memset(work, 0, 16);

	value = eval_disasm_expr(dbgframe, &cmd, &valid, sz, &r);
	if (valid) {
		switch (sz) {
		case 1:
			sprintf(scratchbuf, "=0x%02X", (unsigned char)value);
			break;
		case 2:
			sprintf(scratchbuf, "=0x%04X", (unsigned short)value);
			break;
		case 4:
			sprintf(scratchbuf, "=0x%lX", (unsigned long)value);
			break;
		case 6:
#if IS_ENABLED(CONFIG_X86_64)
			/* FWORD ptrs are 10 bytes x86-64, 6 bytes ia32 */
			if (r && !mdb_copy(work, r, 10))
				sprintf(scratchbuf, BFMT10,
					work[0], work[1], work[2], work[3],
					work[4], work[5], work[6], work[7],
					work[8], work[9]);
#else
			if (r && !mdb_copy(work, r, 6))
				sprintf(scratchbuf, BFMT6,
					work[0], work[1], work[2],
					work[3], work[4], work[5]);
#endif
			else
				sprintf(scratchbuf, "=?");
			break;
		case 8:
			sprintf(scratchbuf, "=0x%llX", value);
			break;
		case 10:
			if (r && !mdb_copy(work, r, 10))
				sprintf(scratchbuf, BFMT10,
					work[0], work[1], work[2],
					work[3], work[4], work[5],
					work[6], work[7], work[8],
					work[9]);
			else
				sprintf(scratchbuf, "=?");
			break;
		case 16:
			if (r && !mdb_copy(work, r, 16))
				sprintf(scratchbuf, BFMT16,
					work[0], work[1], work[2], work[3],
					work[4], work[5], work[6], work[7],
					work[8], work[9], work[10], work[11],
					work[12], work[13], work[14],
					work[15]);
			else
				sprintf(scratchbuf, "=?");
			break;
		default:
			sprintf(scratchbuf, "=0x%lX", (unsigned long)value);
			break;
		}
		oappend(scratchbuf);
	}
}

static inline void bfd_eval_expr(int bytemode, int sizeflag,
				 unsigned char *deref,
				 dbg_regs *dbgframe)
{
	unsigned char *cmd = deref;
	unsigned long long value;
	unsigned long valid;
	unsigned long sz = 4;
	unsigned char *r = NULL;
	unsigned char work[16];

	memset(work, 0, 16);

	switch (bytemode) {
		/* BYTE PTR */
	case b_mode:
		sz = 1;
		break;

		/* WORD PTR */
	case w_mode:
	case dqw_mode:
		sz = 2;
		break;

		/* QWORD PTR */
	case branch_v_mode:
		if (mode_64bit)
			sz = 8;
		/* DWORD PTR */
		else if (sizeflag & DFLAG)
			sz = 4;
		/* WORD PTR */
		else
			sz = 2;
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;

		/*case branch_v_mode:*/
	case v_mode:
	case dq_mode:
		USED_REX(REX_MODE64);
		if (rex & REX_MODE64)
			sz = 8;
		/* DWORD PTR */
		else if ((sizeflag & DFLAG) || bytemode == dq_mode)
			sz = 4;
		/* WORD PTR */
		else
			sz = 2;
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;
		/* DWORD PTR */
	case d_mode:
		sz = 4;
		break;
		/* QWORD PTR */
	case q_mode:
		sz = 8;
		break;
	case m_mode:
		/* QWORD PTR */
		if (mode_64bit)
			sz = 8;
		/* DWORD PTR */
		else
			sz = 4;
		break;
	case f_mode:
		/* FWORD PTR */
		if (sizeflag & DFLAG) {
			used_prefixes |= (prefixes & PREFIX_DATA);
			sz = 6;
		}
		/* DWORD PTR */
		else
			sz = 4;
		break;
		/* TBYTE PTR */
	case t_mode:
		sz = 10;
		break;
		/* XMMWORD PTR */
	case x_mode:
		sz = 16;
		break;
		/* default is DWORD */
	default:
		sz = 4;
		break;
	}

	value = eval_disasm_expr(dbgframe, &cmd, &valid, sz, &r);
	if (valid) {
		switch (sz) {
		case 1:
			sprintf(scratchbuf, "=0x%02X", (unsigned char)value);
			break;
		case 2:
			sprintf(scratchbuf, "=0x%04X", (unsigned short)value);
			break;
		case 4:
			sprintf(scratchbuf, "=0x%lX", (unsigned long)value);
			break;
		case 6:
#if IS_ENABLED(CONFIG_X86_64)
			/* FWORD ptrs 10 bytes x86-64, 6 bytes ia32 */
			if (r && !mdb_copy(work, r, 10))
				sprintf(scratchbuf, BFMT10,
					work[0], work[1], work[2], work[3],
					work[4], work[5], work[6], work[7],
					work[8], work[9]);
#else
			if (r && !mdb_copy(work, r, 6))
				sprintf(scratchbuf, BFMT6,
					work[0], work[1], work[2],
					work[3], work[4], work[5]);
#endif
			else
				sprintf(scratchbuf, "=?");
			break;
		case 8:
			sprintf(scratchbuf, "=0x%llX", value);
			break;
		case 10:
			if (r && !mdb_copy(work, r, 10))
				sprintf(scratchbuf, BFMT10,
					work[0], work[1], work[2],
					work[3], work[4], work[5],
					work[6], work[7], work[8],
					work[9]);
			else
				sprintf(scratchbuf, "=?");
			break;
		case 16:
			if (r && !mdb_copy(work, r, 16))
				sprintf(scratchbuf, BFMT16,
					work[0], work[1], work[2], work[3],
					work[4], work[5], work[6], work[7],
					work[8], work[9], work[10], work[11],
					work[12], work[13], work[14],
					work[15]);
			else
				sprintf(scratchbuf, "=?");
			break;
		default:
			sprintf(scratchbuf, "=0x%lX", (unsigned long)value);
			break;
		}
		oappend(scratchbuf);
	}
}

static inline int bfd_eval_addr(int bytemode, int sizeflag,
				unsigned long addr,
				unsigned long mask,
				dbg_regs *dbgframe,
				const char *sv)
{
	unsigned char *cmd;
	unsigned char *sym_name;
	unsigned long offset;
	unsigned long long value;
	unsigned long valid;
	unsigned long sz = 4;
	unsigned char *r = NULL;
	unsigned char work[16];

	memset(work, 0, 16);

	switch (bytemode) {
		/* BYTE PTR */
	case b_mode:
		sz = 1;
		break;

		/* WORD PTR */
	case w_mode:
	case dqw_mode:
		sz = 2;
		break;

		/* QWORD PTR */
	case branch_v_mode:
		if (mode_64bit)
			sz = 8;
		/* DWORD PTR */
		else if (sizeflag & DFLAG)
			sz = 4;
		/* WORD PTR */
		else
			sz = 2;
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;

		/*case branch_v_mode:*/
	case v_mode:
	case dq_mode:
		USED_REX(REX_MODE64);
		if (rex & REX_MODE64)
			sz = 8;
		/* DWORD PTR */
		else if ((sizeflag & DFLAG) || bytemode == dq_mode)
			sz = 4;
		/* WORD PTR */
		else
			sz = 2;
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;
		/* DWORD PTR */
	case d_mode:
		sz = 4;
		break;
		/* QWORD PTR */
	case q_mode:
		sz = 8;
		break;
	case m_mode:
		/* QWORD PTR */
		if (mode_64bit)
			sz = 8;
		/* DWORD PTR */
		else
			sz = 4;
		break;
	case f_mode:
		/* FWORD PTR */
		if (sizeflag & DFLAG) {
			used_prefixes |= (prefixes & PREFIX_DATA);
			sz = 6;
		}
		/* DWORD PTR */
		else
			sz = 4;
		break;
		/* TBYTE PTR */
	case t_mode:
		sz = 10;
		break;
		/* XMMWORD PTR */
	case x_mode:
		sz = 16;
		break;
		/* default is DWORD */
	default:
		sz = 4;
		break;
	}

	if (mask)
		addr = addr & mask;

	sym_name = get_symbol_value_offset(addr, &offset,
					   &symbuf[0],
					   MAX_SYMBOL_LEN);
	if (sym_name && sym_name[0] != ' ') {
		cmd = obufp;
		if (sv) {
			oappend(sv);
			oappend(":");
		}
		oappend("[");
		sprintf(scratchbuf, "%s", sym_name);
		oappend(scratchbuf);
		if (offset) {
			sprintf(scratchbuf, "+0x%lx", offset);
			oappend(scratchbuf);
		}
		oappend("]");

		value = eval_disasm_expr(dbgframe, &cmd, &valid,
					 sz, &r);
		if (valid) {
			switch (sz) {
			case 1:
				sprintf(scratchbuf, "=0x%02X",
					(unsigned char)value);
				break;
			case 2:
				sprintf(scratchbuf, "=0x%04X",
					(unsigned short)value);
				break;
			case 4:
				sprintf(scratchbuf, "=0x%lX",
					(unsigned long)value);
				break;
			case 6:
#if IS_ENABLED(CONFIG_X86_64)
				/*FWORD ptrs 10 bytes x86-64, 6 bytes ia32*/
				if (r && !mdb_copy(work, r, 10))
					sprintf(scratchbuf, BFMT10,
						work[0], work[1], work[2],
						work[3], work[4], work[5],
						work[6], work[7],
						work[8], work[9]);
#else
				if (r && !mdb_copy(work, r, 6))
					sprintf(scratchbuf, BFMT6,
						work[0], work[1], work[2],
						work[3], work[4], work[5]);
#endif
				else
					sprintf(scratchbuf, "=?");
				break;
			case 8:
				sprintf(scratchbuf, "=0x%llX", value);
				break;
			case 10:
				if (r && !mdb_copy(work, r, 10))
					sprintf(scratchbuf, BFMT10,
						work[0], work[1], work[2],
						work[3], work[4], work[5],
						work[6], work[7], work[8],
						work[9]);
				else
					sprintf(scratchbuf, "=?");
				break;
			case 16:
				if (r && !mdb_copy(work, r, 16))
					sprintf(scratchbuf, BFMT16,
						work[0], work[1], work[2],
						work[3], work[4], work[5],
						work[6], work[7], work[8],
						work[9], work[10], work[11],
						work[12], work[13], work[14],
						work[15]);
				else
					sprintf(scratchbuf, "=?");
				break;
			default:
				sprintf(scratchbuf, "=0x%lX",
					(unsigned long)value);
				break;
			}
			oappend(scratchbuf);
		}
		return 1;
	}
	return 0;
}

static int print_insn(mdb_vma pc, struct disassemble_info *info,
		      dbg_regs *dbgframe)
{
	const struct dis386 *dp;
	int i;
	char *first, *second, *third;
	int needcomma;
	unsigned char uses_SSE_prefix, uses_LOCK_prefix;
	int sizeflag;
	const char *p;
	struct dis_private priv;

	mode_64bit = (info->mach == mach_x86_64_intel_syntax ||
		      info->mach == mach_x86_64);

	if (intel_syntax == (char)-1)
		intel_syntax = (info->mach ==
				mach_i386_i386_intel_syntax ||
				info->mach == mach_x86_64_intel_syntax);

	if (info->mach == mach_i386_i386 ||
	    info->mach == mach_x86_64 ||
	    info->mach == mach_i386_i386_intel_syntax ||
	    info->mach == mach_x86_64_intel_syntax)
		priv.orig_sizeflag = AFLAG | DFLAG;
	else if (info->mach == mach_i386_i8086)
		priv.orig_sizeflag = 0;
	else
		abort();

	for (p = info->disassembler_options; p; ) {
		if (strncmp(p, "x86-64", 6) == 0) {
			mode_64bit = 1;
			priv.orig_sizeflag = AFLAG | DFLAG;
		} else if (strncmp(p, "i386", 4) == 0) {
			mode_64bit = 0;
			priv.orig_sizeflag = AFLAG | DFLAG;
		} else if (strncmp(p, "i8086", 5) == 0) {
			mode_64bit = 0;
			priv.orig_sizeflag = 0;
		} else if (strncmp(p, "intel", 5) == 0) {
			intel_syntax = 1;
		} else if (strncmp(p, "att", 3) == 0) {
			intel_syntax = 0;
		} else if (strncmp(p, "addr", 4) == 0) {
			if (p[4] == '1' && p[5] == '6')
				priv.orig_sizeflag &= ~AFLAG;
			else if (p[4] == '3' && p[5] == '2')
				priv.orig_sizeflag |= AFLAG;
		} else if (strncmp(p, "data", 4) == 0) {
			if (p[4] == '1' && p[5] == '6')
				priv.orig_sizeflag &= ~DFLAG;
			else if (p[4] == '3' && p[5] == '2')
				priv.orig_sizeflag |= DFLAG;
		} else if (strncmp(p, "suffix", 6) == 0) {
			priv.orig_sizeflag |= SUFFIX_ALWAYS;
		}

		p = strchr(p, ',');
		if (p)
			p++;
	}

	if (intel_syntax) {
		names64 = intel_names64;
		names32 = intel_names32;
		names16 = intel_names16;
		names8 = intel_names8;
		names8rex = intel_names8rex;
		names_seg = intel_names_seg;
		index16 = intel_index16;
		open_char = '[';
		close_char = ']';
		separator_char = '+';
		scale_char = '*';
	} else {
		names64 = att_names64;
		names32 = att_names32;
		names16 = att_names16;
		names8 = att_names8;
		names8rex = att_names8rex;
		names_seg = att_names_seg;
		index16 = att_index16;
		open_char = '(';
		close_char =  ')';
		separator_char = ',';
		scale_char = ',';
	}

	/* The output looks better if we put 7 bytes on a line,
	 * since that puts most long word instructions on a single
	 * line.
	 */
	info->bytes_per_line = 7;

	info->private_data = &priv;
	priv.max_fetched = priv.the_buffer;
	priv.insn_start = pc;

	obuf[0] = 0;
	op1out[0] = 0;
	op2out[0] = 0;
	op3out[0] = 0;

	op_index[0] = -1;
	op_index[1] = -1;
	op_index[2] = -1;

	the_info = info;
	start_pc = pc;
	start_codep = priv.the_buffer;
	codep = priv.the_buffer;

	obufp = obuf;
	ckprefix();

	insn_codep = codep;
	sizeflag = priv.orig_sizeflag;

	FETCH_DATA(info, codep + 1);
	two_source_ops = (*codep == 0x62) || (*codep == 0xc8);

	if ((prefixes & PREFIX_FWAIT) &&
	    ((*codep < 0xd8) || (*codep > 0xdf))) {
		const char *name;

		/* fwait not followed by floating point instruction.
		 * Print the first prefix, which is probably fwait
		 * itself.
		 */
		name = prefix_name(priv.the_buffer[0], priv.orig_sizeflag);
		if (!name)
			name = INTERNAL_DISASSEMBLER_ERROR;
		(*info->fprintf_func) (info->stream, "%s", name);
		return 1;
	}

	if (*codep == 0x0f) {
		FETCH_DATA(info, codep + 2);
		dp = &dis386_twobyte[*++codep];
		need_modrm = twobyte_has_modrm[*codep];
		uses_SSE_prefix = twobyte_uses_SSE_prefix[*codep];
		uses_LOCK_prefix = (*codep & ~0x02) == 0x20;
	} else {
		dp = &dis386[*codep];
		need_modrm = onebyte_has_modrm[*codep];
		uses_SSE_prefix = 0;
		uses_LOCK_prefix = 0;
	}
	codep++;

	if (!uses_SSE_prefix && (prefixes & PREFIX_REPZ)) {
		oappend("repz ");
		used_prefixes |= PREFIX_REPZ;
	}
	if (!uses_SSE_prefix && (prefixes & PREFIX_REPNZ)) {
		oappend("repnz ");
		used_prefixes |= PREFIX_REPNZ;
	}
	if (!uses_LOCK_prefix && (prefixes & PREFIX_LOCK)) {
		oappend("lock ");
		used_prefixes |= PREFIX_LOCK;
	}

	if (prefixes & PREFIX_ADDR) {
		sizeflag ^= AFLAG;
		if (dp->bytemode3 != loop_jcxz_mode || intel_syntax) {
			if ((sizeflag & AFLAG) || mode_64bit)
				oappend("addr32 ");
			else
				oappend("addr16 ");
			used_prefixes |= PREFIX_ADDR;
		}
	}

	if (!uses_SSE_prefix && (prefixes & PREFIX_DATA)) {
		sizeflag ^= DFLAG;
		if (dp->bytemode3 == cond_jump_mode &&
		    dp->bytemode1 == v_mode && !intel_syntax) {
			if (sizeflag & DFLAG)
				oappend("data32 ");
			else
				oappend("data16 ");
			used_prefixes |= PREFIX_DATA;
		}
	}

	if (need_modrm) {
		FETCH_DATA(info, codep + 1);
		mod = (*codep >> 6) & 3;
		reg = (*codep >> 3) & 7;
		rm = *codep & 7;
	}

	if (!dp->name && dp->bytemode1 == FLOATCODE) {
		dofloat (sizeflag, dbgframe);
	} else {
		int index;

		if (!dp->name) {
			switch (dp->bytemode1) {
			case USE_GROUPS:
				dp = &grps[dp->bytemode2][reg];
				break;

			case USE_PREFIX_TABLE:
				index = 0;
				used_prefixes |= (prefixes & PREFIX_REPZ);
				if (prefixes & PREFIX_REPZ) {
					index = 1;
					dp = &prefix_tbl[dp->bytemode2][index];
					break;
				}
				used_prefixes |= (prefixes & PREFIX_DATA);
				if (prefixes & PREFIX_DATA) {
					index = 2;
				} else {
					used_prefixes |=
						(prefixes & PREFIX_REPNZ);
					if (prefixes & PREFIX_REPNZ)
						index = 3;
				}
				dp = &prefix_tbl[dp->bytemode2][index];
				break;

			case X86_64_SPECIAL:
				dp = &x86_64_table[dp->bytemode2][mode_64bit];
				break;

			default:
				oappend(INTERNAL_DISASSEMBLER_ERROR);
				break;
			}
		}

		if (putop(dp->name, sizeflag) == 0) {
			obufp = op1out;
			op_ad = 2;
			if (dp->op1)
				(*dp->op1) (dp->bytemode1, sizeflag,
					    dbgframe);

			obufp = op2out;
			op_ad = 1;
			if (dp->op2)
				(*dp->op2) (dp->bytemode2, sizeflag,
					    dbgframe);

			obufp = op3out;
			op_ad = 0;
			if (dp->op3)
				(*dp->op3) (dp->bytemode3, sizeflag,
					    dbgframe);
		}
	}

	/* See if any prefixes were not used.	 If so, print the first one
	 * separately.  If we don't do this, we'll wind up printing an
	 * instruction stream which does not precisely correspond to the
	 * bytes we are disassembling.
	 */
	if ((prefixes & ~used_prefixes) != 0) {
		const char *name;

		name = prefix_name(priv.the_buffer[0], priv.orig_sizeflag);
		if (!name)
			name = INTERNAL_DISASSEMBLER_ERROR;
		(*info->fprintf_func) (info->stream, "%s", name);
		return 1;
	}
	if (rex & ~rex_used) {
		const char *name;

		name = prefix_name(rex | 0x40, priv.orig_sizeflag);
		if (!name)
			name = INTERNAL_DISASSEMBLER_ERROR;
		(*info->fprintf_func) (info->stream, "%s ", name);
	}

	obufp = obuf + strlen(obuf);
	for (i = strlen(obuf); i < 6; i++)
		oappend(" ");
	oappend(" ");
	(*info->fprintf_func) (info->stream, "%s", obuf);

	/* The enter and bound instructions are printed with operands
	 * in the same order as the intel book; everything else is printed
	 * in reverse order.
	 */
	if (intel_syntax || two_source_ops) {
		int riprel;

		first = op1out;
		second = op2out;
		third = op3out;

		op_ad = op_index[0];
		op_index[0] = op_index[2];
		op_index[2] = op_ad;

		riprel = op_riprel[0];
		op_riprel[0] = op_riprel[2];
		op_riprel[2] = riprel;
	} else {
		first = op3out;
		second = op2out;
		third = op1out;
	}
	needcomma = 0;
	if (*first) {
		if (op_index[0] != -1 && !op_riprel[0]) {
			(*info->print_address_func)
				((mdb_vma)op_address[op_index[0]], info);
			if (intel_syntax || two_source_ops)
				output_jmp_address(dbgframe,
						   op_address[op_index[0]],
						   obuf, info);
		} else {
			(*info->fprintf_func) (info->stream, "%s", first);
		}
		needcomma = 1;
	}
	if (*second) {
		if (needcomma)
			(*info->fprintf_func) (info->stream, ",");
		if (op_index[1] != -1 && !op_riprel[1]) {
			(*info->fprintf_func) (info->stream, "S:");
			(*info->print_address_func)
				((mdb_vma)op_address[op_index[1]], info);
		} else {
			(*info->fprintf_func) (info->stream, "%s", second);
		}
		needcomma = 1;
	}
	if (*third) {
		if (needcomma)
			(*info->fprintf_func) (info->stream, ",");
		if (op_index[2] != -1 && !op_riprel[2]) {
			(*info->print_address_func)
				((mdb_vma)op_address[op_index[2]], info);
			if (!intel_syntax && !two_source_ops)
				output_jmp_address(dbgframe,
						   op_address[op_index[0]],
						   obuf, info);
		} else {
			(*info->fprintf_func) (info->stream, "%s", third);
		}
	}
	return codep - priv.the_buffer;
}

const char *float_mem[] = {
	/* d8 */
	"fadd{s||s|}",
	"fmul{s||s|}",
	"fcom{s||s|}",
	"fcomp{s||s|}",
	"fsub{s||s|}",
	"fsubr{s||s|}",
	"fdiv{s||s|}",
	"fdivr{s||s|}",
	/* d9 */
	"fld{s||s|}",
	"(bad)",
	"fst{s||s|}",
	"fstp{s||s|}",
	"fldenvIC",
	"fldcw",
	"fNstenvIC",
	"fNstcw",
	/* da */
	"fiadd{l||l|}",
	"fimul{l||l|}",
	"ficom{l||l|}",
	"ficomp{l||l|}",
	"fisub{l||l|}",
	"fisubr{l||l|}",
	"fidiv{l||l|}",
	"fidivr{l||l|}",
	/* db */
	"fild{l||l|}",
	"fisttp{l||l|}",
	"fist{l||l|}",
	"fistp{l||l|}",
	"(bad)",
	"fld{t||t|}",
	"(bad)",
	"fstp{t||t|}",
	/* dc */
	"fadd{l||l|}",
	"fmul{l||l|}",
	"fcom{l||l|}",
	"fcomp{l||l|}",
	"fsub{l||l|}",
	"fsubr{l||l|}",
	"fdiv{l||l|}",
	"fdivr{l||l|}",
	/* dd */
	"fld{l||l|}",
	"fisttp{ll||ll|}",
	"fst{l||l|}",
	"fstp{l||l|}",
	"frstorIC",
	"(bad)",
	"fNsaveIC",
	"fNstsw",
	/* de */
	"fiadd",
	"fimul",
	"ficom",
	"ficomp",
	"fisub",
	"fisubr",
	"fidiv",
	"fidivr",
	/* df */
	"fild",
	"fisttp",
	"fist",
	"fistp",
	"fbld",
	"fild{ll||ll|}",
	"fbstp",
	"fistp{ll||ll|}",
};

static const unsigned char float_mem_mode[] = {
	/* d8 */
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	/* d9 */
	d_mode,
	0,
	d_mode,
	d_mode,
	0,
	w_mode,
	0,
	w_mode,
	/* da */
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	/* db */
	d_mode,
	d_mode,
	d_mode,
	d_mode,
	0,
	t_mode,
	0,
	t_mode,
	/* dc */
	q_mode,
	q_mode,
	q_mode,
	q_mode,
	q_mode,
	q_mode,
	q_mode,
	q_mode,
	/* dd */
	q_mode,
	q_mode,
	q_mode,
	q_mode,
	0,
	0,
	0,
	w_mode,
	/* de */
	w_mode,
	w_mode,
	w_mode,
	w_mode,
	w_mode,
	w_mode,
	w_mode,
	w_mode,
	/* df */
	w_mode,
	w_mode,
	w_mode,
	w_mode,
	t_mode,
	q_mode,
	t_mode,
	q_mode
};

#define FGRPD9_2 { NULL, NULL, 0, NULL, 0, NULL, 0 }
#define FGRPD9_4 { NULL, NULL, 1, NULL, 0, NULL, 0 }
#define FGRPD9_5 { NULL, NULL, 2, NULL, 0, NULL, 0 }
#define FGRPD9_6 { NULL, NULL, 3, NULL, 0, NULL, 0 }
#define FGRPD9_7 { NULL, NULL, 4, NULL, 0, NULL, 0 }
#define FGRPDA_5 { NULL, NULL, 5, NULL, 0, NULL, 0 }
#define FGRPDB_4 { NULL, NULL, 6, NULL, 0, NULL, 0 }
#define FGRPDE_3 { NULL, NULL, 7, NULL, 0, NULL, 0 }
#define FGRPDF_4 { NULL, NULL, 8, NULL, 0, NULL, 0 }

static const struct dis386 float_reg[][8] = {
	/* d8 */
	{
		{ "fadd",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fmul",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcom",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "fcomp",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "fsub",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fsubr",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fdiv",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fdivr",	OP_ST, 0, OP_STI, 0, NULL, 0 },
	},
	/* d9 */
	{
		{ "fld",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "fxch",	OP_STI, 0, NULL, 0, NULL, 0 },
		FGRPD9_2,
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		FGRPD9_4,
		FGRPD9_5,
		FGRPD9_6,
		FGRPD9_7,
	},
	/* da */
	{
		{ "fcmovb",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcmove",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcmovbe",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcmovu",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		FGRPDA_5,
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* db */
	{
		{ "fcmovnb",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcmovne",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcmovnbe",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcmovnu",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		FGRPDB_4,
		{ "fucomi",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcomi",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* dc */
	{
		{ "fadd",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fmul",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
#if UNIXWARE_COMPAT
		{ "fsub",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fsubr",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fdiv",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fdivr",	OP_STI, 0, OP_ST, 0, NULL, 0 },
#else
		{ "fsubr",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fsub",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fdivr",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fdiv",	OP_STI, 0, OP_ST, 0, NULL, 0 },
#endif
	},
	/* dd */
	{
		{ "ffree",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "fst",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "fstp",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "fucom",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "fucomp",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
	/* de */
	{
		{ "faddp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fmulp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		FGRPDE_3,
#if UNIXWARE_COMPAT
		{ "fsubp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fsubrp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fdivp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fdivrp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
#else
		{ "fsubrp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fsubp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fdivrp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
		{ "fdivp",	OP_STI, 0, OP_ST, 0, NULL, 0 },
#endif
	},
	/* df */
	{
		{ "ffreep",	OP_STI, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
		FGRPDF_4,
		{ "fucomip",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "fcomip",	OP_ST, 0, OP_STI, 0, NULL, 0 },
		{ "(bad)",	NULL, 0, NULL, 0, NULL, 0 },
	},
};

static char *fgrps[][8] = {
	/* d9_2  0 */
	{
		"fnop", "(bad)", "(bad)", "(bad)", "(bad)", "(bad)",
		"(bad)", "(bad)",
	},

	/* d9_4  1 */
	{
		"fchs", "fabs", "(bad)", "(bad)", "ftst", "fxam",
		"(bad)", "(bad)",
	},

	/* d9_5  2 */
	{
		"fld1", "fldl2t", "fldl2e", "fldpi", "fldlg2", "fldln2",
		"fldz", "(bad)",
	},

	/* d9_6  3 */
	{
		"f2xm1", "fyl2x", "fptan", "fpatan", "fxtract", "fprem1",
		"fdecstp", "fincstp",
	},

	/* d9_7  4 */
	{
		"fprem", "fyl2xp1", "fsqrt", "fsincos", "frndint", "fscale",
		"fsin", "fcos",
	},

	/* da_5  5 */
	{
		"(bad)", "fucompp", "(bad)", "(bad)", "(bad)", "(bad)",
		"(bad)", "(bad)",
	},

	/* db_4  6 */
	{
		"feni(287 only)", "fdisi(287 only)", "fNclex", "fNinit",
		"fNsetpm(287 only)", "(bad)", "(bad)", "(bad)",
	},

	/* de_3  7 */
	{
		"(bad)", "fcompp", "(bad)", "(bad)", "(bad)", "(bad)",
		"(bad)", "(bad)",
	},

	/* df_4  8 */
	{
		"fNstsw", "(bad)", "(bad)", "(bad)", "(bad)", "(bad)",
		"(bad)", "(bad)",
	},
};

static void dofloat (int sizeflag, dbg_regs *dbgframe)
{
	const struct dis386 *dp;
	unsigned char floatop;

	floatop = codep[-1];

	if (mod != 3) {
		int fp_indx = (floatop - 0xd8) * 8 + reg;

		putop(float_mem[fp_indx], sizeflag);
		obufp = op1out;
		OP_E(float_mem_mode[fp_indx], sizeflag, dbgframe);
		return;
	}
	/* Skip mod/rm byte.	*/
	MODRM_CHECK;
	codep++;

	dp = &float_reg[floatop - 0xd8][reg];
	if (!dp->name) {
		putop(fgrps[dp->bytemode1][rm], sizeflag);

		/* Instruction fnstsw is only one with strange arg. */
		if (floatop == 0xdf && codep[-1] == 0xe0)
			strcpy(op1out, names16[0]);
	} else {
		putop(dp->name, sizeflag);

		obufp = op1out;
		if (dp->op1)
			(*dp->op1) (dp->bytemode1, sizeflag, dbgframe);
		obufp = op2out;
		if (dp->op2)
			(*dp->op2) (dp->bytemode2, sizeflag, dbgframe);
	}
}

static void OP_ST(int bytemode ATTR_UNUSED, int sizeflag ATTR_UNUSED,
		  dbg_regs *dbgframe)
{
	oappend("%st");
}

static void OP_STI(int bytemode ATTR_UNUSED, int sizeflag ATTR_UNUSED,
		   dbg_regs *dbgframe)
{
	sprintf(scratchbuf, "%%st(%d)", rm);
	oappend(scratchbuf + intel_syntax);
}

/* Capital letters in template are macros.  */
static int putop(const char *template, int sizeflag)
{
	const char *p;
	int alt = 0;

	for (p = template; *p; p++) {
		switch (*p) {
		default:
			*obufp++ = *p;
			break;
		case '{':
			alt = 0;
			if (intel_syntax)
				alt += 1;
			if (mode_64bit)
				alt += 2;
			while (alt != 0) {
				while (*++p != '|') {
					if (*p == '}') {
						/* Alternative not valid. */
						strcpy(obuf, "(bad)");
						obufp = obuf + 5;
						return 1;
					} else if (*p == '\0') {
						abort();
					}
				}
				alt--;
			}
			/* Fall through.  */
		case 'I':
			alt = 1;
			continue;
		case '|':
			while (*++p != '}') {
				if (*p == '\0')
					abort();
			}
			break;
		case '}':
			break;
		case 'A':
			if (intel_syntax)
				break;
			if (mod != 3 || (sizeflag & SUFFIX_ALWAYS))
				*obufp++ = 'b';
			break;
		case 'B':
			if (intel_syntax)
				break;
			if (sizeflag & SUFFIX_ALWAYS)
				*obufp++ = 'b';
			break;
		case 'C':
			if (intel_syntax && !alt)
				break;
			if ((prefixes & PREFIX_DATA) ||
			    (sizeflag & SUFFIX_ALWAYS)) {
				if (sizeflag & DFLAG)
					*obufp++ = intel_syntax ? 'd' : 'l';
				else
					*obufp++ = intel_syntax ? 'w' : 's';
				used_prefixes |= (prefixes & PREFIX_DATA);
			}
			break;
		case 'E':		/* For jcxz/jecxz */
			if (mode_64bit) {
				if (sizeflag & AFLAG)
					*obufp++ = 'r';
				else
					*obufp++ = 'e';
			} else if (sizeflag & AFLAG) {
				*obufp++ = 'e';
			}
			used_prefixes |= (prefixes & PREFIX_ADDR);
			break;
		case 'F':
			if (intel_syntax)
				break;
			if ((prefixes & PREFIX_ADDR) ||
			    (sizeflag & SUFFIX_ALWAYS)) {
				if (sizeflag & AFLAG)
					*obufp++ = mode_64bit ? 'q' : 'l';
				else
					*obufp++ = mode_64bit ? 'l' : 'w';
				used_prefixes |= (prefixes & PREFIX_ADDR);
			}
			break;
		case 'H':
			if (intel_syntax)
				break;
			if ((prefixes &
			     (PREFIX_CS | PREFIX_DS)) == PREFIX_CS ||
			    (prefixes & (PREFIX_CS | PREFIX_DS)) ==
			    PREFIX_DS) {
				used_prefixes |=
					prefixes & (PREFIX_CS | PREFIX_DS);
				*obufp++ = ',';
				*obufp++ = 'p';
				if (prefixes & PREFIX_DS)
					*obufp++ = 't';
				else
					*obufp++ = 'n';
			}
			break;
		case 'J':
			if (intel_syntax)
				break;
			*obufp++ = 'l';
			break;
		case 'L':
			if (intel_syntax)
				break;
			if (sizeflag & SUFFIX_ALWAYS)
				*obufp++ = 'l';
			break;
		case 'N':
			if ((prefixes & PREFIX_FWAIT) == 0)
				*obufp++ = 'n';
			else
				used_prefixes |= PREFIX_FWAIT;
			break;
		case 'O':
			USED_REX(REX_MODE64);
			if (rex & REX_MODE64)
				*obufp++ = 'o';
			else
				*obufp++ = 'd';
			break;
		case 'T':
			if (intel_syntax)
				break;
			if (mode_64bit) {
				*obufp++ = 'q';
				break;
			}
			/* Fall through.  */
		case 'P':
			if (intel_syntax)
				break;
			if ((prefixes & PREFIX_DATA) || (rex & REX_MODE64) ||
			    (sizeflag & SUFFIX_ALWAYS)) {
				USED_REX(REX_MODE64);
				if (rex & REX_MODE64) {
					*obufp++ = 'q';
				} else {
					if (sizeflag & DFLAG)
						*obufp++ = 'l';
					else
						*obufp++ = 'w';
					used_prefixes |=
						(prefixes & PREFIX_DATA);
				}
			}
			break;
		case 'U':
			if (intel_syntax)
				break;
			if (mode_64bit) {
				*obufp++ = 'q';
				break;
			}
			/* Fall through.  */
		case 'Q':
			if (intel_syntax && !alt)
				break;
			USED_REX(REX_MODE64);
			if (mod != 3 || (sizeflag & SUFFIX_ALWAYS)) {
				if (rex & REX_MODE64) {
					*obufp++ = 'q';
				} else {
					if (sizeflag & DFLAG)
						*obufp++ =
							intel_syntax ?
							'd' : 'l';
					else
						*obufp++ = 'w';
					used_prefixes |=
						(prefixes & PREFIX_DATA);
				}
			}
			break;
		case 'R':
			USED_REX(REX_MODE64);
			if (intel_syntax) {
				if (rex & REX_MODE64) {
					*obufp++ = 'q';
					*obufp++ = 't';
				} else if (sizeflag & DFLAG) {
					*obufp++ = 'd';
					*obufp++ = 'q';
				} else {
					*obufp++ = 'w';
					*obufp++ = 'd';
				}
			} else {
				if (rex & REX_MODE64)
					*obufp++ = 'q';
				else if (sizeflag & DFLAG)
					*obufp++ = 'l';
				else
					*obufp++ = 'w';
			}
			if (!(rex & REX_MODE64))
				used_prefixes |= (prefixes & PREFIX_DATA);
			break;
		case 'S':
			if (intel_syntax)
				break;
			if (sizeflag & SUFFIX_ALWAYS) {
				if (rex & REX_MODE64) {
					*obufp++ = 'q';
				} else {
					if (sizeflag & DFLAG)
						*obufp++ = 'l';
					else
						*obufp++ = 'w';
					used_prefixes |=
						(prefixes & PREFIX_DATA);
				}
			}
			break;
		case 'X':
			if (prefixes & PREFIX_DATA)
				*obufp++ = 'd';
			else
				*obufp++ = 's';
			used_prefixes |= (prefixes & PREFIX_DATA);
			break;
		case 'Y':
			if (intel_syntax)
				break;
			if (rex & REX_MODE64) {
				USED_REX(REX_MODE64);
				*obufp++ = 'q';
			}
			break;
			/* implicit operand size 'l' for
			 *  i386 or 'q' for x86-64
			 */
		case 'W':
			/* operand size flag for cwtl, cbtw */
			USED_REX(0);
			if (rex)
				*obufp++ = 'l';
			else if (sizeflag & DFLAG)
				*obufp++ = 'w';
			else
				*obufp++ = 'b';
			if (intel_syntax) {
				if (rex) {
					*obufp++ = 'q';
					*obufp++ = 'e';
				}
				if (sizeflag & DFLAG) {
					*obufp++ = 'd';
					*obufp++ = 'e';
				} else {
					*obufp++ = 'w';
				}
			}
			if (!rex)
				used_prefixes |= (prefixes & PREFIX_DATA);
			break;
		}
		alt = 0;
	}
	*obufp = 0;
	return 0;
}

static void oappend(const char *s)
{
	strcpy(obufp, s);
	obufp += strlen(s);
}

static void append_seg(void)
{
	if (prefixes & PREFIX_CS) {
		used_prefixes |= PREFIX_CS;
		oappend("%cs:" + intel_syntax);
	}
	if (prefixes & PREFIX_DS) {
		used_prefixes |= PREFIX_DS;
		oappend("%ds:" + intel_syntax);
	}
	if (prefixes & PREFIX_SS) {
		used_prefixes |= PREFIX_SS;
		oappend("%ss:" + intel_syntax);
	}
	if (prefixes & PREFIX_ES) {
		used_prefixes |= PREFIX_ES;
		oappend("%es:" + intel_syntax);
	}
	if (prefixes & PREFIX_FS) {
		used_prefixes |= PREFIX_FS;
		oappend("%fs:" + intel_syntax);
	}
	if (prefixes & PREFIX_GS) {
		used_prefixes |= PREFIX_GS;
		oappend("%gs:" + intel_syntax);
	}
}

static void OP_INDIRE(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	if (!intel_syntax)
		oappend("*");
	OP_E(bytemode, sizeflag, dbgframe);
}

static void print_operand_value(char *buf, int hex, mdb_vma disp)
{
	if (mode_64bit) {
		if (hex) {
			char tmp[30];
			int i;

			buf[0] = '0';
			buf[1] = 'x';
			mdb_sprintf_vma(tmp, disp);
			for (i = 0; tmp[i] == '0' && tmp[i + 1]; i++)
				i = i;
			strcpy(buf + 2, tmp + i);
		} else {
			mdb_signed_vma v = disp;
			char tmp[30];
			int i;

			if (v < 0) {
				*(buf++) = '-';
				v = -disp;
				/* Check for possible overflow
				 *  on 0x8000000000000000.
				 */
				if (v < 0) {
					strcpy(buf, "9223372036854775808");
					return;
				}
			}
			if (!v) {
				strcpy(buf, "0");
				return;
			}

			i = 0;
			tmp[29] = 0;
			while (v) {
				tmp[28 - i] = (v % 10) + '0';
				v /= 10;
				i++;
			}
			strcpy(buf, tmp + 29 - i);
		}
	} else {
		if (hex)
			sprintf(buf, "0x%x", (unsigned int)disp);
		else
			sprintf(buf, "%d", (int)disp);
	}
}

static void OP_E(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	unsigned char *deref;

	mdb_vma disp;
	int add = 0;
	int riprel = 0;

	USED_REX(REX_EXTZ);
	if (rex & REX_EXTZ)
		add += 8;

	/* Skip mod/rm byte.	*/
	MODRM_CHECK;
	codep++;

	if (mod == 3) {
		switch (bytemode) {
		case b_mode:
			USED_REX(0);
			if (rex)
				oappend(names8rex[rm + add]);
			else
				oappend(names8[rm + add]);
			break;
		case w_mode:
			oappend(names16[rm + add]);
			break;
		case d_mode:
			oappend(names32[rm + add]);
			break;
		case q_mode:
			oappend(names64[rm + add]);
			break;
		case m_mode:
			if (mode_64bit)
				oappend(names64[rm + add]);
			else
				oappend(names32[rm + add]);
			break;
		case branch_v_mode:
			if (mode_64bit) {
				oappend(names64[rm + add]);
			} else {
				if ((sizeflag & DFLAG) ||
				    bytemode != branch_v_mode)
					oappend(names32[rm + add]);
				else
					oappend(names16[rm + add]);
				used_prefixes |= (prefixes & PREFIX_DATA);
			}
			break;
		case v_mode:
		case dq_mode:
		case dqw_mode:
			USED_REX(REX_MODE64);
			if (rex & REX_MODE64)
				oappend(names64[rm + add]);
			else if ((sizeflag & DFLAG) || bytemode != v_mode)
				oappend(names32[rm + add]);
			else
				oappend(names16[rm + add]);
			used_prefixes |= (prefixes & PREFIX_DATA);
			break;
		case 0:
			break;
		default:
			oappend(INTERNAL_DISASSEMBLER_ERROR);
			break;
		}
		return;
	}

	disp = 0;
	append_seg();

	/* 32 bit address mode */
	if ((sizeflag & AFLAG) || mode_64bit) {
		int havesib;
		int havebase;
		int base;
		int index = 0;
		int scale = 0;

		havesib = 0;
		havebase = 1;
		base = rm;

		if (base == 4) {
			havesib = 1;
			FETCH_DATA(the_info, codep + 1);
			index = (*codep >> 3) & 7;
			if (mode_64bit || index != 0x4)
				/* When INDEX == 0x4 in 32
				 * bit mode, SCALE is
				 * ignored.
				 */
				scale = (*codep >> 6) & 3;
			base = *codep & 7;
			USED_REX(REX_EXTY);
			if (rex & REX_EXTY)
				index += 8;
			codep++;
		}
		base += add;

		switch (mod) {
		case 0:
			if ((base & 7) == 5) {
				havebase = 0;
				if (mode_64bit && !havesib)
					riprel = 1;
				disp = get32s();
			}
			break;
		case 1:
			FETCH_DATA(the_info, codep + 1);
			disp = *codep++;
			if ((disp & 0x80) != 0)
				disp -= 0x100;
			break;
		case 2:
			disp = get32s();
			break;
		}

		if ((!intel_syntax) && (mod != 0 || (base & 7) == 5)) {
			if (riprel) {
				set_op(disp, 1);
				if (!bfd_eval_addr(bytemode, sizeflag,
						   start_pc + codep -
						   start_codep +
						   op_address[op_index[op_ad]],
						   0, dbgframe, NULL)) {
					print_operand_value(scratchbuf, 1,
							    disp);
					oappend(scratchbuf);
					oappend("(%rip)");
				}
			} else {
				print_operand_value(scratchbuf, 1, disp);
				oappend(scratchbuf);
			}
		}

		if ((intel_syntax) && (mod != 0 || (base & 7) == 5)) {
			if (riprel) {
				set_op(disp, 1);
				if (!bfd_eval_addr(bytemode, sizeflag,
						   start_pc + codep -
						   start_codep +
						   op_address[op_index[op_ad]],
						   0, dbgframe, NULL)) {
					oappend("[rip+");
					print_operand_value(scratchbuf, 1,
							    disp);
					oappend(scratchbuf);
					oappend("]");
				}
			}
		}

		if (havebase || (havesib && (index != 4 || scale != 0))) {
			if (intel_syntax) {
				switch (bytemode) {
				case b_mode:
					oappend("BYTE PTR ");
					break;
				case w_mode:
				case dqw_mode:
					oappend("WORD PTR ");
					break;
				case branch_v_mode:
					if (mode_64bit)
						oappend("QWORD PTR ");
					else if (sizeflag & DFLAG)
						oappend("DWORD PTR ");
					else
						oappend("WORD PTR ");
					used_prefixes |=
						(prefixes & PREFIX_DATA);
					break;
					/*case branch_v_mode:*/
				case v_mode:
				case dq_mode:
					USED_REX(REX_MODE64);
					if (rex & REX_MODE64)
						oappend("QWORD PTR ");
					else if ((sizeflag & DFLAG) ||
						 bytemode == dq_mode)
						oappend("DWORD PTR ");
					else
						oappend("WORD PTR ");
					used_prefixes |=
						(prefixes & PREFIX_DATA);
					break;
				case d_mode:
					oappend("DWORD PTR ");
					break;
				case q_mode:
					oappend("QWORD PTR ");
					break;
				case m_mode:
					if (mode_64bit)
						oappend("QWORD PTR ");
					else
						oappend("DWORD PTR ");
					break;
				case f_mode:
					if (sizeflag & DFLAG) {
						used_prefixes |=
							(prefixes &
							 PREFIX_DATA);
						oappend("FWORD PTR ");
					} else {
						oappend("DWORD PTR ");
					}
					break;
				case t_mode:
					oappend("TBYTE PTR ");
					break;
				case x_mode:
					oappend("XMMWORD PTR ");
					break;
				default:
					break;
				}
			}

			/* begin dereference */
			deref = obufp;
			*obufp++ = open_char;
			*obufp = '\0';
			if (havebase) {
				oappend(mode_64bit && (sizeflag & AFLAG)
					? names64[base] : names32[base]);
			}
			if (havesib) {
				if (index != 4) {
					if (!intel_syntax || havebase) {
						*obufp++ = separator_char;
						*obufp = '\0';
					}
					/* check the bottom case and bracket
					 * if true
					 */
					if (scale != 0)
						*obufp++ = '(';
					oappend(mode_64bit &&
						(sizeflag & AFLAG)
						? names64[index]
						: names32[index]);
				}
				if (scale != 0 ||
				    (!intel_syntax && index != 4)) {
					*obufp++ = scale_char;
					*obufp = '\0';
					sprintf(scratchbuf,
						"%d", 1 << scale);
					oappend(scratchbuf);

					/* if we fell through the top case,
					 * close the bracket
					 */
					if (index != 4)
						*obufp++ = ')';
				}
			}
			if (intel_syntax && disp) {
				if ((mdb_signed_vma)disp > 0) {
					*obufp++ = '+';
					*obufp = '\0';
				} else if (mod != 1) {
					*obufp++ = '-';
					*obufp = '\0';
					disp = -(mdb_signed_vma)disp;
				}

				print_operand_value(scratchbuf, mod != 1, disp);
				oappend(scratchbuf);
			}

			/* end dereference */
			*obufp++ = close_char;
			*obufp = '\0';
			if (intel_syntax)
				bfd_eval_expr(bytemode, sizeflag, deref,
					      dbgframe);
		} else if ((intel_syntax) && (mod != 0 || (base & 7) == 5) &&
			   (!riprel)) {
			if (prefixes &
			    (PREFIX_CS | PREFIX_SS | PREFIX_DS |
			     PREFIX_ES | PREFIX_FS | PREFIX_GS)) {
				if (!bfd_eval_addr(bytemode, sizeflag,
						   disp, 0,
						   dbgframe, NULL)) {
					print_operand_value(scratchbuf,
							    1, disp);
					oappend(scratchbuf);
				}
			} else {
				if (!bfd_eval_addr(bytemode, sizeflag,
						   disp, 0,
						   dbgframe,
						   names_seg[ds_reg -
						   es_reg])) {
					print_operand_value(scratchbuf,
							    1, disp);
					oappend(scratchbuf);
				}
			}
		}
	} else { /* 16 bit address mode */
		switch (mod) {
		case 0:
			if (rm == 6) {
				disp = get16();
				if ((disp & 0x8000) != 0)
					disp -= 0x10000;
			}
			break;
		case 1:
			FETCH_DATA(the_info, codep + 1);
			disp = *codep++;
			if ((disp & 0x80) != 0)
				disp -= 0x100;
			break;
		case 2:
			disp = get16();
			if ((disp & 0x8000) != 0)
				disp -= 0x10000;
			break;
		}

		if (!intel_syntax)
			if (mod != 0 || rm == 6) {
				print_operand_value(scratchbuf, 0, disp);
				oappend(scratchbuf);
			}

		if (mod != 0 || rm != 6) {
			/* begin dereference */
			deref = obufp;
			*obufp++ = open_char;
			*obufp = '\0';
			oappend(index16[rm]);
			if (intel_syntax && disp) {
				if ((mdb_signed_vma)disp > 0) {
					*obufp++ = '+';
					*obufp = '\0';
				} else if (mod != 1) {
					*obufp++ = '-';
					*obufp = '\0';
					disp = -(mdb_signed_vma)disp;
				}

				print_operand_value(scratchbuf, mod != 1, disp);
				oappend(scratchbuf);
			}

			/* end dereference */
			*obufp++ = close_char;
			*obufp = '\0';
			if (intel_syntax)
				bfd_eval_expr(bytemode, sizeflag, deref,
					      dbgframe);
		} else if (intel_syntax) {
			if (prefixes & (PREFIX_CS | PREFIX_SS | PREFIX_DS
					| PREFIX_ES | PREFIX_FS | PREFIX_GS)) {
				if (!bfd_eval_addr(bytemode, sizeflag, disp,
						   0xFFFF, dbgframe, NULL)) {
					print_operand_value(scratchbuf, 1,
							    disp & 0xFFFF);
					oappend(scratchbuf);
				}
			} else {
				if (!bfd_eval_addr(bytemode, sizeflag, disp,
						   0xFFFF, dbgframe,
						   names_seg[ds_reg -
						   es_reg])) {
					print_operand_value(scratchbuf, 1,
							    disp & 0xFFFF);
					oappend(scratchbuf);
				}
			}
		}
	}
}

static void OP_G(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	int add = 0;

	USED_REX(REX_EXTX);
	if (rex & REX_EXTX)
		add += 8;
	switch (bytemode) {
	case b_mode:
		USED_REX(0);
		if (rex)
			oappend(names8rex[reg + add]);
		else
			oappend(names8[reg + add]);
		break;
	case w_mode:
		oappend(names16[reg + add]);
		break;
	case d_mode:
		oappend(names32[reg + add]);
		break;
	case q_mode:
		oappend(names64[reg + add]);
		break;
	case v_mode:
	case dq_mode:
	case dqw_mode:
		USED_REX(REX_MODE64);
		if (rex & REX_MODE64)
			oappend(names64[reg + add]);
		else if ((sizeflag & DFLAG) || bytemode != v_mode)
			oappend(names32[reg + add]);
		else
			oappend(names16[reg + add]);
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;
	case m_mode:
		if (mode_64bit)
			oappend(names64[reg + add]);
		else
			oappend(names32[reg + add]);
		break;
	default:
		oappend(INTERNAL_DISASSEMBLER_ERROR);
		break;
	}
}

static mdb_vma get64(void)
{
	mdb_vma x;
#if defined(BFD64)
	unsigned int a;
	unsigned int b;

	FETCH_DATA(the_info, codep + 8);
	a = *codep++ & 0xff;
	a |= (*codep++ & 0xff) << 8;
	a |= (*codep++ & 0xff) << 16;
	a |= (*codep++ & 0xff) << 24;
	b = *codep++ & 0xff;
	b |= (*codep++ & 0xff) << 8;
	b |= (*codep++ & 0xff) << 16;
	b |= (*codep++ & 0xff) << 24;
	x = a + ((mdb_vma)b << 32);
#else
	abort();
	x = 0;
#endif
	return x;
}

static mdb_signed_vma get32(void)
{
	mdb_signed_vma x = 0;

	FETCH_DATA(the_info, codep + 4);
	x = *codep++ & (mdb_signed_vma)0xff;
	x |= (*codep++ & (mdb_signed_vma)0xff) << 8;
	x |= (*codep++ & (mdb_signed_vma)0xff) << 16;
	x |= (*codep++ & (mdb_signed_vma)0xff) << 24;
	return x;
}

static mdb_signed_vma get32s(void)
{
	mdb_signed_vma x = 0;

	FETCH_DATA(the_info, codep + 4);
	x = *codep++ & (mdb_signed_vma)0xff;
	x |= (*codep++ & (mdb_signed_vma)0xff) << 8;
	x |= (*codep++ & (mdb_signed_vma)0xff) << 16;
	x |= (*codep++ & (mdb_signed_vma)0xff) << 24;

	x = (x ^ ((mdb_signed_vma)1 << 31)) - ((mdb_signed_vma)1 << 31);

	return x;
}

static int get16(void)
{
	int x = 0;

	FETCH_DATA(the_info, codep + 2);
	x = *codep++ & 0xff;
	x |= (*codep++ & 0xff) << 8;
	return x;
}

static void set_op(mdb_vma op, int riprel)
{
	op_index[op_ad] = op_ad;
	if (mode_64bit) {
		op_address[op_ad] = op;
		op_riprel[op_ad] = riprel;
	} else {
		/* Mask to get a 32-bit address.*/
		op_address[op_ad] = op & 0xffffffff;
		op_riprel[op_ad] = riprel & 0xffffffff;
	}
}

static void OP_REG(int code, int sizeflag, dbg_regs *dbgframe)
{
	const char *s;
	int add = 0;

	USED_REX(REX_EXTZ);
	if (rex & REX_EXTZ)
		add = 8;

	switch (code) {
	case indir_dx_reg:
		if (intel_syntax)
			s = "[dx]";
		else
			s = "(%dx)";
		break;
	case ax_reg: case cx_reg: case dx_reg: case bx_reg:
	case sp_reg: case bp_reg: case si_reg: case di_reg:
		s = names16[code - ax_reg + add];
		break;
	case es_reg: case ss_reg: case cs_reg:
	case ds_reg: case fs_reg: case gs_reg:
		s = names_seg[code - es_reg + add];
		break;
	case al_reg: case ah_reg: case cl_reg: case ch_reg:
	case dl_reg: case dh_reg: case bl_reg: case bh_reg:
		USED_REX(0);
		if (rex)
			s = names8rex[code - al_reg + add];
		else
			s = names8[code - al_reg];
		break;
	case rax_reg: case rcx_reg: case rdx_reg: case rbx_reg:
	case rsp_reg: case rbp_reg: case rsi_reg: case rdi_reg:
		if (mode_64bit) {
			s = names64[code - rax_reg + add];
			break;
		}
		code += eax_reg - rax_reg;
		/* Fall through.	*/
	case eax_reg: case ecx_reg: case edx_reg: case ebx_reg:
	case esp_reg: case ebp_reg: case esi_reg: case edi_reg:
		USED_REX(REX_MODE64);
		if (rex & REX_MODE64)
			s = names64[code - eax_reg + add];
		else if (sizeflag & DFLAG)
			s = names32[code - eax_reg + add];
		else
			s = names16[code - eax_reg + add];
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;
	default:
		s = INTERNAL_DISASSEMBLER_ERROR;
		break;
	}
	oappend(s);
}

static void OP_IMREG(int code, int sizeflag, dbg_regs *dbgframe)
{
	const char *s;

	switch (code) {
	case indir_dx_reg:
		if (intel_syntax)
			s = "[dx]";
		else
			s = "(%dx)";
		break;
	case ax_reg: case cx_reg: case dx_reg: case bx_reg:
	case sp_reg: case bp_reg: case si_reg: case di_reg:
		s = names16[code - ax_reg];
		break;
	case es_reg: case ss_reg: case cs_reg:
	case ds_reg: case fs_reg: case gs_reg:
		s = names_seg[code - es_reg];
		break;
	case al_reg: case ah_reg: case cl_reg: case ch_reg:
	case dl_reg: case dh_reg: case bl_reg: case bh_reg:
		USED_REX(0);
		if (rex)
			s = names8rex[code - al_reg];
		else
			s = names8[code - al_reg];
		break;
	case eax_reg: case ecx_reg: case edx_reg: case ebx_reg:
	case esp_reg: case ebp_reg: case esi_reg: case edi_reg:
		USED_REX(REX_MODE64);
		if (rex & REX_MODE64)
			s = names64[code - eax_reg];
		else if (sizeflag & DFLAG)
			s = names32[code - eax_reg];
		else
			s = names16[code - eax_reg];
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;
	default:
		s = INTERNAL_DISASSEMBLER_ERROR;
		break;
	}
	oappend(s);
}

static void OP_I(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	mdb_signed_vma op;
	mdb_signed_vma mask = -1;

	switch (bytemode) {
	case b_mode:
		FETCH_DATA(the_info, codep + 1);
		op = *codep++;
		mask = 0xff;
		break;
	case q_mode:
		if (mode_64bit) {
			op = get32s();
			break;
		}
		/* Fall through.	*/
	case v_mode:
		USED_REX(REX_MODE64);
		if (rex & REX_MODE64) {
			op = get32s();
		} else if (sizeflag & DFLAG) {
			op = get32();
			mask = 0xffffffff;
		} else {
			op = get16();
			mask = 0xfffff;
		}
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;
	case w_mode:
		mask = 0xfffff;
		op = get16();
		break;
	case const_1_mode:
		if (intel_syntax)
			oappend("1");
		return;
	default:
		oappend(INTERNAL_DISASSEMBLER_ERROR);
		return;
	}

	op &= mask;
	scratchbuf[0] = '$';
	print_operand_value(scratchbuf + 1, 1, op);
	oappend(scratchbuf + intel_syntax);
	scratchbuf[0] = '\0';
}

static void OP_I64(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	mdb_signed_vma op;
	mdb_signed_vma mask = -1;

	if (!mode_64bit) {
		OP_I(bytemode, sizeflag, dbgframe);
		return;
	}

	switch (bytemode) {
	case b_mode:
		FETCH_DATA(the_info, codep + 1);
		op = *codep++;
		mask = 0xff;
		break;
	case v_mode:
		USED_REX(REX_MODE64);
		if (rex & REX_MODE64) {
			op = get64();
		} else if (sizeflag & DFLAG) {
			op = get32();
			mask = 0xffffffff;
		} else {
			op = get16();
			mask = 0xfffff;
		}
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;
	case w_mode:
		mask = 0xfffff;
		op = get16();
		break;
	default:
		oappend(INTERNAL_DISASSEMBLER_ERROR);
		return;
	}

	op &= mask;
	scratchbuf[0] = '$';
	print_operand_value(scratchbuf + 1, 1, op);
	oappend(scratchbuf + intel_syntax);
	scratchbuf[0] = '\0';
}

static void OP_SI(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	mdb_signed_vma op;
	mdb_signed_vma mask = -1;

	switch (bytemode) {
	case b_mode:
		FETCH_DATA(the_info, codep + 1);
		op = *codep++;
		if ((op & 0x80) != 0)
			op -= 0x100;
		mask = 0xffffffff;
		break;
	case v_mode:
		USED_REX(REX_MODE64);
		if (rex & REX_MODE64) {
			op = get32s();
		} else if (sizeflag & DFLAG) {
			op = get32s();
			mask = 0xffffffff;
		} else {
			mask = 0xffffffff;
			op = get16();
			if ((op & 0x8000) != 0)
				op -= 0x10000;
		}
		used_prefixes |= (prefixes & PREFIX_DATA);
		break;
	case w_mode:
		op = get16();
		mask = 0xffffffff;
		if ((op & 0x8000) != 0)
			op -= 0x10000;
		break;
	default:
		oappend(INTERNAL_DISASSEMBLER_ERROR);
		return;
	}

	scratchbuf[0] = '$';
	print_operand_value(scratchbuf + 1, 1, op);
	oappend(scratchbuf + intel_syntax);
}

static void OP_J(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	mdb_vma disp;
	mdb_vma mask = -1;

	switch (bytemode) {
	case b_mode:
		FETCH_DATA(the_info, codep + 1);
		disp = *codep++;
		if ((disp & 0x80) != 0)
			disp -= 0x100;
		break;
	case v_mode:
		if (sizeflag & DFLAG) {
			disp = get32s();
		} else {
			disp = get16();
			/* For some reason, a data16 prefix on a
			 * jump instruction means that the pc is
			 * masked to 16 bits after the displacement
			 * is added!
			 */
			mask = 0xffff;
		}
		break;
	default:
		oappend(INTERNAL_DISASSEMBLER_ERROR);
		return;
	}
	disp = (start_pc + codep - start_codep + disp) & mask;
	set_op(disp, 0);
	print_operand_value(scratchbuf, 1, disp);
	oappend(scratchbuf);
}

static void OP_SEG(int dummy ATTR_UNUSED,
		   int sizeflag ATTR_UNUSED,
		   dbg_regs *dbgframe)
{
	oappend(names_seg[reg]);
}

static void OP_DIR(int dummy ATTR_UNUSED, int sizeflag,
		   dbg_regs *dbgframe)
{
	int seg, offset;

	if (sizeflag & DFLAG) {
		offset = get32();
		seg = get16();
	} else {
		offset = get16();
		seg = get16();
	}
	used_prefixes |= (prefixes & PREFIX_DATA);
	if (intel_syntax)
		sprintf(scratchbuf, "0x%x, 0x%x", seg, offset);
	else
		sprintf(scratchbuf, "$0x%x,$0x%x", seg, offset);
	oappend(scratchbuf);
}

static void OP_OFF(int bytemode ATTR_UNUSED,
		   int sizeflag, dbg_regs *dbgframe)
{
	mdb_vma off;

	append_seg();

	if ((sizeflag & AFLAG) || mode_64bit)
		off = get32();
	else
		off = get16();

	if (intel_syntax) {
		if (!(prefixes & (PREFIX_CS | PREFIX_SS | PREFIX_DS
				  | PREFIX_ES | PREFIX_FS | PREFIX_GS))) {
			if (!bfd_eval_addr(bytemode, sizeflag, off,
					   0, dbgframe,
					   names_seg[ds_reg - es_reg])) {
				print_operand_value(scratchbuf, 1, off);
				oappend(scratchbuf);
			}
		} else {
			if (!bfd_eval_addr(bytemode, sizeflag, off,
					   0, dbgframe, NULL)) {
				print_operand_value(scratchbuf, 1, off);
				oappend(scratchbuf);
			}
		}
	} else {
		print_operand_value(scratchbuf, 1, off);
		oappend(scratchbuf);
	}
}

static void OP_OFF64(int bytemode ATTR_UNUSED,
		     int sizeflag ATTR_UNUSED,
		     dbg_regs *dbgframe)
{
	mdb_vma off;

	if (!mode_64bit) {
		OP_OFF(bytemode, sizeflag, dbgframe);
		return;
	}

	append_seg();

	off = get64();

	if (intel_syntax) {
		if (!(prefixes & (PREFIX_CS | PREFIX_SS | PREFIX_DS
				  | PREFIX_ES | PREFIX_FS | PREFIX_GS))) {
			if (!bfd_eval_addr(bytemode, sizeflag, off,
					   0, dbgframe,
					   names_seg[ds_reg - es_reg])) {
				print_operand_value(scratchbuf, 1, off);
				oappend(scratchbuf);
			}
		} else {
			if (!bfd_eval_addr(bytemode, sizeflag, off,
					   0, dbgframe, NULL)) {
				print_operand_value(scratchbuf, 1, off);
				oappend(scratchbuf);
			}
		}
	} else {
		print_operand_value(scratchbuf, 1, off);
		oappend(scratchbuf);
	}
}

static void
ptr_reg(int code, int sizeflag, dbg_regs *dbgframe)
{
	const char *s;
	unsigned char *deref;

	/* begin dereference */
	deref = obufp;
	*obufp++ = open_char;
	used_prefixes |= (prefixes & PREFIX_ADDR);
	if (mode_64bit) {
		if (!(sizeflag & AFLAG))
			s = names32[code - eax_reg];
		else
			s = names64[code - eax_reg];
	} else if (sizeflag & AFLAG) {
		s = names32[code - eax_reg];
	} else {
		s = names16[code - eax_reg];
	}
	oappend(s);
	/* end dereference */
	*obufp++ = close_char;
	*obufp = 0;
	if (intel_syntax) {
		register int size;

		if (mode_64bit) {
			/* 32 bit */
			if (!(sizeflag & AFLAG))
				size = 4;
			/* 64 bit */
			else
				size = 8;
		}
		/* 32 bit */
		else if (sizeflag & AFLAG)
			size = 4;
		/* 16 bit */
		else
			size = 2;

		bfd_eval_size(size, sizeflag, deref, dbgframe);
	}
}

static void
OP_ESREG(int code, int sizeflag, dbg_regs *dbgframe)
{
	if (intel_syntax) {
		if (codep[-1] & 1) {
			USED_REX(REX_MODE64);
			used_prefixes |= (prefixes & PREFIX_DATA);
			if (rex & REX_MODE64)
				oappend("QWORD PTR ");
			else if ((sizeflag & DFLAG))
				oappend("DWORD PTR ");
			else
				oappend("WORD PTR ");
		} else {
			oappend("BYTE PTR ");
		}
	}

	oappend("%es:" + intel_syntax);
	ptr_reg(code, sizeflag, dbgframe);
}

static void OP_DSREG(int code, int sizeflag, dbg_regs *dbgframe)
{
	if (intel_syntax) {
		if (codep[-1] != 0xd7 && (codep[-1] & 1)) {
			USED_REX(REX_MODE64);
			used_prefixes |= (prefixes & PREFIX_DATA);
			if (rex & REX_MODE64)
				oappend("QWORD PTR ");
			else if ((sizeflag & DFLAG))
				oappend("DWORD PTR ");
			else
				oappend("WORD PTR ");
		} else {
			oappend("BYTE PTR ");
		}
	}

	if ((prefixes
	     & (PREFIX_CS
		| PREFIX_DS
		| PREFIX_SS
		| PREFIX_ES
		| PREFIX_FS
		| PREFIX_GS)) == 0)
		prefixes |= PREFIX_DS;
	append_seg();
	ptr_reg(code, sizeflag, dbgframe);
}

static void OP_C(int dummy ATTR_UNUSED,
		 int sizeflag ATTR_UNUSED,
		 dbg_regs *dbgframe)
{
	int add = 0;

	if (rex & REX_EXTX) {
		USED_REX(REX_EXTX);
		add = 8;
	} else if (!mode_64bit && (prefixes & PREFIX_LOCK)) {
		used_prefixes |= PREFIX_LOCK;
		add = 8;
	}
	sprintf(scratchbuf, "%%cr%d", reg + add);
	oappend(scratchbuf + intel_syntax);
}

static void OP_D(int dummy ATTR_UNUSED,
		 int sizeflag ATTR_UNUSED,
		 dbg_regs *dbgframe)
{
	int add = 0;

	USED_REX(REX_EXTX);
	if (rex & REX_EXTX)
		add = 8;
	if (intel_syntax)
		sprintf(scratchbuf, "db%d", reg + add);
	else
		sprintf(scratchbuf, "%%db%d", reg + add);
	oappend(scratchbuf);
}

static void OP_T(int dummy ATTR_UNUSED,
		 int sizeflag ATTR_UNUSED,
		 dbg_regs *dbgframe)
{
	sprintf(scratchbuf, "%%tr%d", reg);
	oappend(scratchbuf + intel_syntax);
}

static void
OP_RD(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	if (mod == 3)
		OP_E(bytemode, sizeflag, dbgframe);
	else
		bad_op();
}

static void OP_MMX(int bytemode ATTR_UNUSED,
		   int sizeflag ATTR_UNUSED,
		   dbg_regs *dbgframe)
{
	used_prefixes |= (prefixes & PREFIX_DATA);
	if (prefixes & PREFIX_DATA) {
		int add = 0;

		USED_REX(REX_EXTX);
		if (rex & REX_EXTX)
			add = 8;
		sprintf(scratchbuf, "%%xmm%d", reg + add);
	} else {
		sprintf(scratchbuf, "%%mm%d", reg);
	}
	oappend(scratchbuf + intel_syntax);
}

static void OP_XMM(int bytemode ATTR_UNUSED,
		   int sizeflag ATTR_UNUSED,
		   dbg_regs *dbgframe)
{
	int add = 0;

	USED_REX(REX_EXTX);
	if (rex & REX_EXTX)
		add = 8;
	sprintf(scratchbuf, "%%xmm%d", reg + add);
	oappend(scratchbuf + intel_syntax);
}

static void
OP_EM(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	if (mod != 3) {
		if (intel_syntax && bytemode == v_mode) {
			bytemode =
				(prefixes & PREFIX_DATA) ? x_mode : q_mode;
			used_prefixes |= (prefixes & PREFIX_DATA);
		}
		OP_E(bytemode, sizeflag, dbgframe);
		return;
	}

	/* Skip mod/rm byte.	*/
	MODRM_CHECK;
	codep++;
	used_prefixes |= (prefixes & PREFIX_DATA);
	if (prefixes & PREFIX_DATA) {
		int add = 0;

		USED_REX(REX_EXTZ);
		if (rex & REX_EXTZ)
			add = 8;
		sprintf(scratchbuf, "%%xmm%d", rm + add);
	} else {
		sprintf(scratchbuf, "%%mm%d", rm);
	}
	oappend(scratchbuf + intel_syntax);
}

static void
OP_EX(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	int add = 0;

	if (mod != 3) {
		if (intel_syntax && bytemode == v_mode) {
			switch (prefixes &
				(PREFIX_DATA | PREFIX_REPZ | PREFIX_REPNZ)) {
			case 0:
				bytemode = x_mode;
				break;
			case PREFIX_REPZ:
				bytemode = d_mode;
				used_prefixes |= PREFIX_REPZ;
				break;
			case PREFIX_DATA:
				bytemode = x_mode;
				used_prefixes |= PREFIX_DATA;
				break;
			case PREFIX_REPNZ:
				bytemode = q_mode;
				used_prefixes |= PREFIX_REPNZ;
				break;
			default:
				bytemode = 0;
				break;
			}
		}
		OP_E(bytemode, sizeflag, dbgframe);
		return;
	}
	USED_REX(REX_EXTZ);
	if (rex & REX_EXTZ)
		add = 8;

	/* Skip mod/rm byte.	*/
	MODRM_CHECK;
	codep++;
	sprintf(scratchbuf, "%%xmm%d", rm + add);
	oappend(scratchbuf + intel_syntax);
}

static void
OP_MS(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	if (mod == 3)
		OP_EM(bytemode, sizeflag, dbgframe);
	else
		bad_op();
}

static void
OP_XS(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	if (mod == 3)
		OP_EX(bytemode, sizeflag, dbgframe);
	else
		bad_op();
}

static void
OP_M(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	if (mod == 3)
		bad_op();	/* bad lea, lds, les, lfs, lgs, lss modrm */
	else
		OP_E(bytemode, sizeflag, dbgframe);
}

static void
OP_0F07(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	if (mod != 3 || rm != 0)
		bad_op();
	else
		OP_E(bytemode, sizeflag, dbgframe);
}

static void
OP_0FAE(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	if (mod == 3) {
		if (reg == 5)
			strcpy(obuf + strlen(obuf) - sizeof("xrstor") + 1,
			       "lfence");
		if (reg == 7)
			strcpy(obuf + strlen(obuf) - sizeof("clflush") + 1,
			       "sfence");

		if (reg < 5 || rm != 0) {
			bad_op();	/* bad sfence, mfence, or lfence */
			return;
		}
	} else if (reg != 5 && reg != 7) {
		bad_op();		/* bad xrstor or clflush */
		return;
	}

	OP_E(bytemode, sizeflag, dbgframe);
}

static void
NOP_FIXUP(int bytemode ATTR_UNUSED, int sizeflag ATTR_UNUSED,
	  dbg_regs *dbgframe)
{
	/* NOP with REPZ prefix is called PAUSE.  */
	if (prefixes == PREFIX_REPZ)
		strcpy(obuf, "pause");
}

static const char *const SUFFIX3DNOW[] = {
	/* 00 */	NULL,	NULL,	NULL,	NULL,
	/* 04 */	NULL,	NULL,	NULL,	NULL,
	/* 08 */	NULL,	NULL,	NULL,	NULL,
	/* 0C */	"pi2fw", "pi2fd", NULL,	NULL,
	/* 10 */	NULL,	NULL,	NULL,	NULL,
	/* 14 */	NULL,	NULL,	NULL,	NULL,
	/* 18 */	NULL,	NULL,	NULL,	NULL,
	/* 1C */	"pf2iw", "pf2id", NULL,	NULL,
	/* 20 */	NULL,	NULL,	NULL,	NULL,
	/* 24 */	NULL,	NULL,	NULL,	NULL,
	/* 28 */	NULL,	NULL,	NULL,	NULL,
	/* 2C */	NULL,	NULL,	NULL,	NULL,
	/* 30 */	NULL,	NULL,	NULL,	NULL,
	/* 34 */	NULL,	NULL,	NULL,	NULL,
	/* 38 */	NULL,	NULL,	NULL,	NULL,
	/* 3C */	NULL,	NULL,	NULL,	NULL,
	/* 40 */	NULL,	NULL,	NULL,	NULL,
	/* 44 */	NULL,	NULL,	NULL,	NULL,
	/* 48 */	NULL,	NULL,	NULL,	NULL,
	/* 4C */	NULL,	NULL,	NULL,	NULL,
	/* 50 */	NULL,	NULL,	NULL,	NULL,
	/* 54 */	NULL,	NULL,	NULL,	NULL,
	/* 58 */	NULL,	NULL,	NULL,	NULL,
	/* 5C */	NULL,	NULL,	NULL,	NULL,
	/* 60 */	NULL,	NULL,	NULL,	NULL,
	/* 64 */	NULL,	NULL,	NULL,	NULL,
	/* 68 */	NULL,	NULL,	NULL,	NULL,
	/* 6C */	NULL,	NULL,	NULL,	NULL,
	/* 70 */	NULL,	NULL,	NULL,	NULL,
	/* 74 */	NULL,	NULL,	NULL,	NULL,
	/* 78 */	NULL,	NULL,	NULL,	NULL,
	/* 7C */	NULL,	NULL,	NULL,	NULL,
	/* 80 */	NULL,	NULL,	NULL,	NULL,
	/* 84 */	NULL,	NULL,	NULL,	NULL,
	/* 88 */	NULL,	NULL,	"pfnacc", NULL,
	/* 8C */	NULL,	NULL,	"pfpnacc", NULL,
	/* 90 */	"pfcmpge", NULL, NULL,	NULL,
	/* 94 */	"pfmin", NULL, "pfrcp",	"pfrsqrt",
	/* 98 */	NULL, NULL, "pfsub", NULL,
	/* 9C */	NULL, NULL, "pfadd", NULL,
	/* A0 */	"pfcmpgt", NULL, NULL,	NULL,
	/* A4 */	"pfmax", NULL, "pfrcpit1", "pfrsqit1",
	/* A8 */	NULL, NULL, "pfsubr", NULL,
	/* AC */	NULL, NULL, "pfacc", NULL,
	/* B0 */	"pfcmpeq", NULL, NULL, NULL,
	/* B4 */	"pfmul", NULL, "pfrcpit2", "pfmulhrw",
	/* B8 */	NULL,	NULL,	NULL,	"pswapd",
	/* BC */	NULL,	NULL,	NULL,	"pavgusb",
	/* C0 */	NULL,	NULL,	NULL,	NULL,
	/* C4 */	NULL,	NULL,	NULL,	NULL,
	/* C8 */	NULL,	NULL,	NULL,	NULL,
	/* CC */	NULL,	NULL,	NULL,	NULL,
	/* D0 */	NULL,	NULL,	NULL,	NULL,
	/* D4 */	NULL,	NULL,	NULL,	NULL,
	/* D8 */	NULL,	NULL,	NULL,	NULL,
	/* DC */	NULL,	NULL,	NULL,	NULL,
	/* E0 */	NULL,	NULL,	NULL,	NULL,
	/* E4 */	NULL,	NULL,	NULL,	NULL,
	/* E8 */	NULL,	NULL,	NULL,	NULL,
	/* EC */	NULL,	NULL,	NULL,	NULL,
	/* F0 */	NULL,	NULL,	NULL,	NULL,
	/* F4 */	NULL,	NULL,	NULL,	NULL,
	/* F8 */	NULL,	NULL,	NULL,	NULL,
	/* FC */	NULL,	NULL,	NULL,	NULL,
};

static void OP_3DNOWSUFFIX(int bytemode ATTR_UNUSED,
			   int sizeflag ATTR_UNUSED,
			   dbg_regs *dbgframe)
{
	const char *mnemonic;

	FETCH_DATA(the_info, codep + 1);
	/* AMD 3DNOW! instructions are specified by an opcode suffix in the
	 * place where an 8-bit immediate would normally go. ie. the last
	 * byte of the instruction.
	 */
	obufp = obuf + strlen(obuf);
	mnemonic = SUFFIX3DNOW[*codep++ & 0xff];
	if (mnemonic) {
		oappend(mnemonic);
	} else {
		/* Since a variable sized modrm/sib chunk is between
		 * the start of the opcode (0x0f0f) and the opcode
		 * suffix, we need to do all the modrm processing first,
		 * and don't know until now that we have a bad opcode.
		 * This necessitates some cleaning up.
		 */
		op1out[0] = '\0';
		op2out[0] = '\0';
		bad_op();
	}
}

const char *simd_cmp_op[] = {
	"eq",
	"lt",
	"le",
	"unord",
	"neq",
	"nlt",
	"nle",
	"ord"
};

static void OP_SIMD_SUFFIX(int bytemode ATTR_UNUSED,
			   int sizeflag ATTR_UNUSED,
			   dbg_regs *dbgframe)
{
	unsigned int cmp_type;

	FETCH_DATA(the_info, codep + 1);
	obufp = obuf + strlen(obuf);
	cmp_type = *codep++ & 0xff;
	if (cmp_type < 8) {
		char suffix1 = 'p', suffix2 = 's';

		used_prefixes |= (prefixes & PREFIX_REPZ);
		if (prefixes & PREFIX_REPZ) {
			suffix1 = 's';
		} else {
			used_prefixes |= (prefixes & PREFIX_DATA);
			if (prefixes & PREFIX_DATA) {
				suffix2 = 'd';
			} else {
				used_prefixes |= (prefixes & PREFIX_REPNZ);
				if (prefixes & PREFIX_REPNZ)
					suffix1 = 's', suffix2 = 'd';
			}
		}
		sprintf(scratchbuf, "cmp%s%c%c",
			simd_cmp_op[cmp_type], suffix1, suffix2);
		used_prefixes |= (prefixes & PREFIX_REPZ);
		oappend(scratchbuf);
	} else {
		/* We have a bad extension byte.	Clean up.  */
		op1out[0] = '\0';
		op2out[0] = '\0';
		bad_op();
	}
}

static void SIMD_FIXUP(int extrachar,
		       int sizeflag ATTR_UNUSED,
		       dbg_regs *dbgframe)
{
	/* Change movlps/movhps to movhlps/movlhps for 2 register
	 * operand forms of these instructions.
	 */
	if (mod == 3) {
		char *p = obuf + strlen(obuf);
		*(p + 1) = '\0';
		*p       = *(p - 1);
		*(p - 1) = *(p - 2);
		*(p - 2) = *(p - 3);
		*(p - 3) = extrachar;
	}
}

static void PNI_FIXUP(int extrachar ATTR_UNUSED,
		      int sizeflag, dbg_regs *dbgframe)
{
	if (mod == 3 && reg == 1 && rm <= 1) {
		/* Override "sidt".  */
		char *p = obuf + strlen(obuf) - 4;

		/* We might have a suffix when disassembling */
		if (*p == 'i')
			--p;

		if (rm) {
			/* mwait %eax,%ecx  */
			strcpy(p, "mwait");
			if (!intel_syntax)
				strcpy(op1out, names32[0]);
		} else {
			/* monitor %eax,%ecx,%edx"  */
			strcpy(p, "monitor");
			if (!intel_syntax) {
				if (!mode_64bit) {
					strcpy(op1out, names32[0]);
				} else if (!(prefixes & PREFIX_ADDR)) {
					strcpy(op1out, names64[0]);
				} else {
					strcpy(op1out, names32[0]);
					used_prefixes |= PREFIX_ADDR;
				}
				strcpy(op3out, names32[2]);
			}
		}
		if (!intel_syntax) {
			strcpy(op2out, names32[1]);
			two_source_ops = 1;
		}

		codep++;
	} else {
		OP_M(0, sizeflag, dbgframe);
	}
}

static void SVME_FIXUP(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	const char *alt;
	char *p;
	unsigned char *deref;

	switch (*codep) {
	case 0xd8:
		alt = "vmrun";
		break;
	case 0xd9:
		alt = "vmmcall";
		break;
	case 0xda:
		alt = "vmload";
		break;
	case 0xdb:
		alt = "vmsave";
		break;
	case 0xdc:
		alt = "stgi";
		break;
	case 0xdd:
		alt = "clgi";
		break;
	case 0xde:
		alt = "skinit";
		break;
	case 0xdf:
		alt = "invlpga";
		break;
	default:
		OP_M(bytemode, sizeflag, dbgframe);
		return;
	}
	/* Override "lidt".  */
	p = obuf + strlen(obuf) - 4;
	/* We might have a suffix.  */
	if (*p == 'i')
		--p;
	strcpy(p, alt);
	if (!(prefixes & PREFIX_ADDR)) {
		++codep;
		return;
	}
	used_prefixes |= PREFIX_ADDR;
	switch (*codep++) {
	case 0xdf:
		strcpy(op2out, names32[1]);
		two_source_ops = 1;
		/* Fall through.  */
	case 0xd8:
	case 0xda:
	case 0xdb:
		/* begin dereference */
		deref = obufp;
		*obufp++ = open_char;
		if (mode_64bit || (sizeflag & AFLAG))
			alt = names32[0];
		else
			alt = names16[0];
		strcpy(obufp, alt);
		obufp += strlen(alt);
		/* end dereference */
		*obufp++ = close_char;
		*obufp = '\0';
		if (intel_syntax)
			bfd_eval_expr(bytemode, sizeflag, deref,
				      dbgframe);
		break;
	}
}

static void INVLPG_FIXUP(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	const char *alt;

	switch (*codep) {
	case 0xf8:
		alt = "swapgs";
		break;
	case 0xf9:
		alt = "rdtscp";
		break;
	default:
		OP_M(bytemode, sizeflag, dbgframe);
		return;
	}
	/* Override "invlpg".  */
	strcpy(obuf + strlen(obuf) - 6, alt);
	codep++;
}

static void bad_op(void)
{
	/* Throw away prefixes and 1st. opcode byte.  */
	codep = insn_codep + 1;
	oappend("(bad)");
}

static void SEG_FIXUP(int extrachar, int sizeflag, dbg_regs *dbgframe)
{
	if (mod == 3) {
		/* We need to add a proper suffix with
		 *
		 * movw %ds,%ax
		 * movl %ds,%eax
		 * movq %ds,%rax
		 * movw %ax,%ds
		 * movl %eax,%ds
		 * movq %rax,%ds
		 */
		const char *suffix;

		if (prefixes & PREFIX_DATA) {
			suffix = "w";
		} else {
			USED_REX(REX_MODE64);
			if (rex & REX_MODE64)
				suffix = "q";
			else
				suffix = "l";
		}
		strcat(obuf, suffix);
	} else {
		/* We need to fix the suffix for
		 *
		 * movw %ds, (%eax)
		 * movw %ds, (%rax)
		 * movw (%eax),%ds
		 * movw (%rax),%ds
		 * Override "mov[l|q]".
		 */
		char *p = obuf + strlen(obuf) - 1;

		/* We might not have a suffix.  */
		if (*p == 'v')
			++p;
		*p = 'w';
	}

	OP_E(extrachar, sizeflag, dbgframe);
}

static void VMX_FIXUP(int extrachar ATTR_UNUSED,
		      int sizeflag, dbg_regs *dbgframe)
{
	if (mod == 3 && reg == 0 && rm >= 1 && rm <= 4) {
		/* Override "sgdt".  */
		char *p = obuf + strlen(obuf) - 4;

		/* We might have a suffix when disassembling */
		if (*p == 'g')
			--p;

		switch (rm) {
		case 1:
			strcpy(p, "vmcall");
			break;
		case 2:
			strcpy(p, "vmlaunch");
			break;
		case 3:
			strcpy(p, "vmresume");
			break;
		case 4:
			strcpy(p, "vmxoff");
			break;
		}

		codep++;
	} else {
		OP_E(0, sizeflag, dbgframe);
	}
}

static void OP_VMX(int bytemode, int sizeflag, dbg_regs *dbgframe)
{
	used_prefixes |= (prefixes & (PREFIX_DATA | PREFIX_REPZ));
	if (prefixes & PREFIX_DATA)
		strcpy(obuf, "vmclear");
	else if (prefixes & PREFIX_REPZ)
		strcpy(obuf, "vmxon");
	else
		strcpy(obuf, "vmptrld");
	OP_E(bytemode, sizeflag, dbgframe);
}

static int mdb_dis_getsym(mdb_vma addr, struct disassemble_info *dip)
{
	return 0;
}

static void mdb_printaddress(unsigned long addr, struct disassemble_info *dip,
			     int flag, int type)
{
	int spaces = 5;
	unsigned char *sym_name;
	unsigned long offset;

	sym_name = get_symbol_value_offset(addr, &offset,
					   &symbuf[0],
					   MAX_SYMBOL_LEN);
	if (sym_name && sym_name[0] != ' ') {
		if (type)
			dip->fprintf_func(dip->stream, "0x%0*lx %s",
					  (int)(2 * sizeof(addr)), addr,
					  sym_name);
		else
			dip->fprintf_func(dip->stream, "%s", sym_name);
		if ((offset) == 0) {
			spaces += 4;
		} else {
			unsigned int o = offset;

			while (o >>= 4)
				--spaces;
			dip->fprintf_func(dip->stream, "+0x%lx", offset);
		}
	} else {
		dip->fprintf_func(dip->stream, "0x%lx", addr);
	}

	if (flag) {
		if (spaces < 1)
			spaces = 1;
		dip->fprintf_func(dip->stream, ":%*s", spaces, " ");
	}
}

int mdb_fprintf(char *out, const char *fmt, ...)
{
	char *work = &fworkbuf[0];
	va_list ap;

	*work = '\0';
	va_start(ap, fmt);
	vsprintf(work, fmt, ap);
	va_end(ap);

	strcat(out, work);

	return 0;
}

int mdb_id_printinsn_intel(unsigned long pc, struct disassemble_info *dip,
			   dbg_regs *dbgframe)
{
	register int err;
	char *final = &finalbuf[0];
	long segment, offset;

	/* print the address info based on architecture */
	if ((dip->mach == mach_i386_i386) ||
	    (dip->mach == mach_x86_64)) {
		mdb_fprintf(final, "0x%p ", (void *)pc);
	} else
		if (dip->mach == mach_i386_i8086) {
			segment = (((unsigned long)pc >> 4) & 0x0000FFFF);
			offset = ((unsigned long)pc & 0x0000000F);

			mdb_fprintf(final, "%04X:%04X ",
				    segment, offset);
		}

	/* perform disassembly */
	err = print_insn_i386_intel(pc, dip, dbgframe);

	/* output byte opcodes */
	mdb_fprintf(final, "%s ", &fbytebuf[0]);

	do {
		mdb_fprintf(final, " ");
		columns++;
	} while (columns < 15);

	/* output disassembly results */
	mdb_fprintf(final, "%s ", &disbuf[0]);

	columns += strlen(&disbuf[0]);
	do {
		mdb_fprintf(final, " ");
		columns++;
	} while (columns < 45);

	return err;
}

int mdb_id_printinsn_att(unsigned long pc, struct disassemble_info *dip,
			 dbg_regs *dbgframe)
{
	mdb_printaddress(pc, dip, 1, 1);
	return print_insn_i386_att(pc, dip, dbgframe);
}

static void mdb_intel_printaddr(mdb_vma addr, struct disassemble_info *dip)
{
	mdb_printaddress(addr, dip, 0, 0);
}

static void mdb_dis_printaddr(mdb_vma addr, struct disassemble_info *dip)
{
	mdb_printaddress(addr, dip, 0, 1);
}

unsigned short read_memory(void *addr, void *buf, unsigned int length)
{
	register unsigned long i;
	register unsigned char *s = buf;

	for (i = 0; i < length; i++) {
		if (s) {
			s[i] = (unsigned char)
				mdb_getword((unsigned long)
					    ((unsigned long)addr + i), 1);
		}
	}
	return 0;
}

static int mdb_dis_getmem(mdb_vma addr, mdb_byte *buf, unsigned int length,
			  struct disassemble_info *dip)
{
	register unsigned long i;
	register unsigned char *s = buf;

	for (i = 0; i < length; i++) {
		if (s) {
			s[i] = (unsigned char)
				mdb_getword((unsigned long)
					    ((unsigned long)addr + i), 1);
			vaddr = (void *)((unsigned long)vaddr + 1);
		}
	}
	return 0;
}

static int mdb_intel_getmem(mdb_vma addr, mdb_byte *buf, unsigned int length,
			    struct disassemble_info *dip)
{
	register unsigned long i;
	register unsigned char *s = buf, *fbyte = (char *)&fbytebuf[0];

	for (i = 0; i < length; i++) {
		if (s) {
			s[i] = (unsigned char)
				mdb_getword((unsigned long)
					    ((unsigned long)addr + i), 1);
			vaddr = (void *)((unsigned long)vaddr + 1);

			mdb_fprintf(fbyte, "%02X", s[i]);
			columns += 2;
		}
	}
	return 0;
}

int mdb_dis_fprintf(void *file, const char *fmt, ...)
{
	va_list ap;

	fworkbuf[0] = '\0';

	va_start(ap, fmt);
	vsprintf(&fworkbuf[0], fmt, ap);
	va_end(ap);

	strcat(&finalbuf[0], &fworkbuf[0]);

	return 0;
}

int mdb_intel_fprintf(void *file, const char *fmt, ...)
{
	va_list ap;

	fworkbuf[0] = '\0';

	va_start(ap, fmt);
	vsprintf(&fworkbuf[0], fmt, ap);
	va_end(ap);

	strcat(&disbuf[0], &fworkbuf[0]);

	return 0;
}

int mdb_dis_fprintf_dummy(void *file, const char *fmt, ...)
{
	return 0;
}

int mdb_id_init(struct disassemble_info *dip, unsigned long type)
{
	dip->stream		= NULL;
	dip->private_data	= NULL;
	dip->bytes_per_line	= 0;

	if (type) {
		dip->read_memory_func       = mdb_dis_getmem;
		dip->print_address_func     = mdb_dis_printaddr;
		dip->fprintf_func           = mdb_dis_fprintf;
	} else {
		dip->read_memory_func       = mdb_intel_getmem;
		dip->print_address_func     = mdb_intel_printaddr;
		dip->fprintf_func           = mdb_intel_fprintf;
	}

	dip->symbol_at_address_func = mdb_dis_getsym;
#if IS_ENABLED(CONFIG_X86_64)
	dip->mach		    = mach_x86_64;
#endif
#if IS_ENABLED(CONFIG_X86_32)
	dip->mach		    = mach_i386_i386;
#endif

	return 0;
}

unsigned long unassemble(dbg_regs *dbgframe, unsigned long ip,
			 unsigned long use, unsigned long *ret,
			 unsigned long type)
{
	jmp_active = 0;
	trap_disable = 0;
	columns = 0;
	needs_proceed = 0;
	disbuf[0] = '\0';
	fbytebuf[0] = '\0';
	finalbuf[0] = '\0';

	mdb_id_init(&mdb_di, type);
	switch (use) {
	case 2:
		mdb_di.mach = mach_x86_64;
		break;

	case 1:
		mdb_di.mach = mach_i386_i386;
		break;

	case 0:
		mdb_di.mach = mach_i386_i8086;
		break;

	default:
#if IS_ENABLED(CONFIG_X86_64)
		mdb_di.mach = mach_x86_64;
#else
		mdb_di.mach = mach_i386_i386;
#endif
		break;
	}

	vaddr = (void *)ip;
	if (type)
		(void)mdb_id_printinsn_att(ip, &mdb_di, dbgframe);
	else
		(void)mdb_id_printinsn_intel(ip, &mdb_di, dbgframe);

	if (!strncasecmp(&disbuf[0], "call", 4) ||
	    !strncasecmp(&disbuf[0], "int", 3)  ||
	    !strncasecmp(&disbuf[0], "loop", 4)) {
		needs_proceed = 1;
		jmp_active = 1;
	}

	if (!strncasecmp(&disbuf[0], "ret", 3))
		jmp_active = 1;

	if (!strncasecmp(&disbuf[0], "iret", 4) ||
	    !strncasecmp(&disbuf[0], "sysexit", 7) ||
	    !strncasecmp(&disbuf[0], "syscall", 7) ||
	    !strncasecmp(&disbuf[0], "sysret", 6))
		jmp_active = 1;

	if (strstr(&disbuf[0], "sysexit") ||
	    strstr(&disbuf[0], "syscall") ||
	    strstr(&disbuf[0], "sysret"))
		trap_disable = 1;

	if (ret)
		*ret = (unsigned long)vaddr;

	if (mdb_printf("%s", &finalbuf[0]))
		return 1;
	if (mdb_printf("\n"))
		return 1;

	return 0;
}
