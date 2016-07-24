
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

#if IS_ENABLED(CONFIG_X86_64)
#define PROCESSOR_WIDTH     64
#else
#define PROCESSOR_WIDTH     32
#endif

#define INVALID  0
#define NUMERIC  1
#define BOOLEAN  2

#define SEG_STACK_SIZE      256
#define NUM_STACK_SIZE      256
#define CONTEXT_STACK_SIZE  1024
#define BOOL_STACK_SIZE     256
#define LOGICAL_STACK_SIZE  256

/* 0-127 token values, 8 high bit is reserved as dereference flag */
#define NULL_TOKEN            0
#define NUMBER_TOKEN          1
#define MINUS_TOKEN           2
#define PLUS_TOKEN            3
#define MULTIPLY_TOKEN        4
#define DIVIDE_TOKEN          5
#define GREATER_TOKEN         6
#define LESS_TOKEN            7
#define XOR_TOKEN             8
#define AND_TOKEN             9
#define OR_TOKEN             10
#define NOT_TOKEN            11
#define NEG_TOKEN            12
#define EQUAL_TOKEN          13
#define LEFT_SHIFT_TOKEN     14
#define RIGHT_SHIFT_TOKEN    15
#define SPACE_TOKEN          16
#define FLAGS_TOKEN          17
#define AX_TOKEN             18
#define BX_TOKEN             19
#define CX_TOKEN             20
#define DX_TOKEN             21
#define SI_TOKEN             22
#define DI_TOKEN             23
#define BP_TOKEN             24
#define SP_TOKEN             25
#define CS_TOKEN             26
#define DS_TOKEN             27
#define ES_TOKEN             28
#define FS_TOKEN             29
#define GS_TOKEN             30
#define SS_TOKEN             31
#define DREF_OPEN_TOKEN      32
#define DREF_CLOSE_TOKEN     33
#define MOD_TOKEN            34
#define NUMBER_END           35
#define GREATER_EQUAL_TOKEN  36
#define LESS_EQUAL_TOKEN     37
#define IP_TOKEN             38
#define ASSIGNMENT_TOKEN     39
#define DWORD_TOKEN          40
#define WORD_TOKEN           41
#define BYTE_TOKEN           42
#define LOGICAL_AND_TOKEN    43
#define LOGICAL_OR_TOKEN     44
#define CF_TOKEN             45
#define PF_TOKEN             46
#define AF_TOKEN             47
#define ZF_TOKEN             48
#define SF_TOKEN             49
#define IF_TOKEN             50
#define DF_TOKEN             51
#define OF_TOKEN             52
#define VM_TOKEN             53
#define AC_TOKEN             54
#define BB_TOKEN             55
#define EB_TOKEN             56
#define NOT_EQUAL_TOKEN      57
#define INVALID_NUMBER_TOKEN 58
#define QWORD_TOKEN          59
#define R8_TOKEN             60
#define R9_TOKEN             61
#define R10_TOKEN            62
#define R11_TOKEN            63
#define R12_TOKEN            64
#define R13_TOKEN            65
#define R14_TOKEN            66
#define R15_TOKEN            67
#define CS_ADDR_TOKEN        68
#define DS_ADDR_TOKEN        69
#define ES_ADDR_TOKEN        70
#define FS_ADDR_TOKEN        71
#define GS_ADDR_TOKEN        72
#define SS_ADDR_TOKEN        73
#define EAX_TOKEN            74
#define EBX_TOKEN            75
#define ECX_TOKEN            76
#define EDX_TOKEN            77
#define RAX_TOKEN            78
#define RBX_TOKEN            79
#define RCX_TOKEN            80
#define RDX_TOKEN            81
#define AL_TOKEN             82
#define BL_TOKEN             83
#define CL_TOKEN             84
#define DL_TOKEN             85
#define ESI_TOKEN            86
#define EDI_TOKEN	     87
#define EBP_TOKEN            88
#define ESP_TOKEN            89
#define RSI_TOKEN            90
#define RDI_TOKEN            91
#define RBP_TOKEN            92
#define RSP_TOKEN            93

/* limit is 0-127 */

#define CF_FLAG   0x00000001
#define PF_FLAG   0x00000004
#define AF_FLAG   0x00000010
#define ZF_FLAG   0x00000040
#define SF_FLAG   0x00000080
#define TF_FLAG   0x00000100  /* ss flag */
#define IF_FLAG   0x00000200
#define DF_FLAG   0x00000400
#define OF_FLAG   0x00000800
#define NT_FLAG   0x00004000
#define RF_FLAG   0x00010000  /* resume flag */
#define VM_FLAG   0x00020000
#define AC_FLAG   0x00040000
#define VIF_FLAG  0x00080000
#define VIP_FLAG  0x00100000
#define ID_FLAG   0x00200000

#define   ARCH_PTR          0
#define   ULONG_PTR         1
#define   WORD_PTR          2
#define   BYTE_PTR          3
#define   ULONGLONG_PTR     4

#define   CLASS_DATA        1
#define   CLASS_ASSIGN      2
#define   CLASS_PARTITION   3
#define   CLASS_ARITHMETIC  4
#define   CLASS_BOOLEAN     5

unsigned char *expr_description[] = {
	"INVALID",
	"NUMERIC",
	"BOOLEAN",
	"???????",
};

unsigned char *parser_description[] = {
	"NULL_TOKEN",
	"NUMBER_TOKEN",
	"MINUS_TOKEN",
	"PLUS_TOKEN",
	"MULTIPLY_TOKEN",
	"DIVIDE_TOKEN",
	"GREATER_TOKEN",
	"LESS_TOKEN",
	"XOR_TOKEN",
	"AND_TOKEN",
	"OR_TOKEN",
	"NOT_TOKEN",
	"NEG_TOKEN",
	"EQUAL_TOKEN",
	"LEFT_SHIFT_TOKEN",
	"RIGHT_SHIFT_TOKEN",
	"SPACE_TOKEN",
	"FLAGS_TOKEN",
	"AX_TOKEN",
	"BX_TOKEN",
	"CX_TOKEN",
	"DX_TOKEN",
	"SI_TOKEN",
	"DI_TOKEN",
	"BP_TOKEN",
	"SP_TOKEN",
	"CS_TOKEN",
	"DS_TOKEN",
	"ES_TOKEN",
	"FS_TOKEN",
	"GS_TOKEN",
	"SS_TOKEN",
	"DREF_OPEN_TOKEN",
	"DREF_CLOSE_TOKEN",
	"MOD_TOKEN",
	"NUMBER_END",
	"GREATER_EQUAL_TOKEN",
	"LESS_EQUAL_TOKEN",
	"IP_TOKEN",
	"ASSIGNMENT_TOKEN",
	"DWORD_TOKEN",
	"WORD_TOKEN",
	"BYTE_TOKEN",
	"LOGICAL_AND_TOKEN",
	"LOGICAL_OR_TOKEN",
	"CF_TOKEN",
	"PF_TOKEN",
	"AF_TOKEN",
	"ZF_TOKEN",
	"SF_TOKEN",
	"IF_TOKEN",
	"DF_TOKEN",
	"OF_TOKEN",
	"VM_TOKEN",
	"AC_TOKEN",
	"BEGIN_BRACKET",
	"END_BRACKET",
	"NOT_EQUAL_TOKEN",
	"INVALID_NUMBER_TOKEN",
	"QWORD_TOKEN",
	"R8_TOKEN",
	"R9_TOKEN",
	"R10_TOKEN",
	"R11_TOKEN",
	"R12_TOKEN",
	"R13_TOKEN",
	"R14_TOKEN",
	"R15_TOKEN",
	"CS_ADDR_TOKEN",
	"DS_ADDR_TOKEN",
	"ES_ADDR_TOKEN",
	"FS_ADDR_TOKEN",
	"GS_ADDR_TOKEN",
	"SS_ADDR_TOKEN",
	"EAX_TOKEN",
	"EBX_TOKEN",
	"ECX_TOKEN",
	"EDX_TOKEN",
	"RAX_TOKEN",
	"RBX_TOKEN",
	"RCX_TOKEN",
	"RDX_TOKEN",
	"AL_TOKEN",
	"BL_TOKEN",
	"CL_TOKEN",
	"DL_TOKEN",
	"ESI_TOKEN",
	"EDI_TOKEN",
	"EBP_TOKEN",
	"ESP_TOKEN",
	"RSI_TOKEN",
	"RDI_TOKEN",
	"RBP_TOKEN",
	"RSP_TOKEN",
};

struct dbgframe_symbols {
	char *symbol;
	int symbol_len;
	unsigned long type;
};

struct dbgframe_symbols dbgframe_symbols[] = {
	{ "CS",  2, CS_TOKEN },
	{ "CS:", 3, CS_ADDR_TOKEN },
	{ "DS",  2, DS_TOKEN },
	{ "DS:", 3, DS_ADDR_TOKEN },
	{ "ES",  2, ES_TOKEN },
	{ "ES:", 3, ES_ADDR_TOKEN },
	{ "FS",  2, FS_TOKEN },
	{ "FS:", 3, FS_ADDR_TOKEN },
	{ "GS",  2, GS_TOKEN },
	{ "GS:", 3, GS_ADDR_TOKEN },
	{ "SS",  2, SS_TOKEN },
	{ "SS:", 3, SS_ADDR_TOKEN },

	{ "AL", 2, AL_TOKEN },
	{ "BL", 2, BL_TOKEN },
	{ "CL", 2, CL_TOKEN },
	{ "DL", 2, DL_TOKEN },

	{ "AX", 2, AX_TOKEN },
	{ "BX", 2, BX_TOKEN },
	{ "CX", 2, CX_TOKEN },
	{ "DX", 2, DX_TOKEN },
	{ "SI", 2, SI_TOKEN },
	{ "DI", 2, DI_TOKEN },
	{ "BP", 2, BP_TOKEN },
	{ "SP", 2, SP_TOKEN },
	{ "IP", 2, IP_TOKEN },

	{ "EAX", 3, EAX_TOKEN },
	{ "EBX", 3, EBX_TOKEN },
	{ "ECX", 3, ECX_TOKEN },
	{ "EDX", 3, EDX_TOKEN },
	{ "ESI", 3, ESI_TOKEN },
	{ "EDI", 3, EDI_TOKEN },
	{ "EBP", 3, EBP_TOKEN },
	{ "ESP", 3, ESP_TOKEN },

	{ "EIP", 3, IP_TOKEN },

#if IS_ENABLED(CONFIG_X86_64)
	{ "RAX", 3, RAX_TOKEN },
	{ "RBX", 3, RBX_TOKEN },
	{ "RCX", 3, RCX_TOKEN },
	{ "RDX", 3, RDX_TOKEN },
	{ "RSI", 3, RSI_TOKEN },
	{ "RDI", 3, RDI_TOKEN },
	{ "RBP", 3, RBP_TOKEN },
	{ "RSP", 3, RSP_TOKEN },
	{ "RIP", 3, IP_TOKEN },
	{ "R8",  2, R8_TOKEN },
	{ "R9",  2, R9_TOKEN },
	{ "R10", 3, R10_TOKEN },
	{ "R11", 3, R11_TOKEN },
	{ "R12", 3, R12_TOKEN },
	{ "R13", 3, R13_TOKEN },
	{ "R14", 3, R14_TOKEN },
	{ "R15", 3, R15_TOKEN },
#endif
	{ "PF", 2, PF_TOKEN },
	{ "ZF", 2, ZF_TOKEN },
	{ "IF", 2, IF_TOKEN },
	{ "OF", 2, OF_TOKEN },
	{ "VM",	2, VM_TOKEN },
	{ "AF",	2, AF_TOKEN },
	{ "AC",	2, AC_TOKEN },
	{ "CF",	2, CF_TOKEN },
	{ "DF",	2, DF_TOKEN },
	{ "SF",	2, SF_TOKEN },
};

static unsigned char token_index[256];
static unsigned char token_type[256];
static unsigned long long token_value[256];
static unsigned long long token_count;

static unsigned long long num_stack[NUM_STACK_SIZE];
static unsigned long long *sp;
static unsigned long long *tos;
static unsigned long long *bos;

static unsigned long long seg_stack[SEG_STACK_SIZE];
static unsigned long long *s_sp;
static unsigned long long *s_tos;
static unsigned long long *s_bos;

static unsigned long long context_stack[CONTEXT_STACK_SIZE];
static unsigned long long *c_sp;
static unsigned long long *c_tos;
static unsigned long long *c_bos;

static unsigned long long boolean_stack[BOOL_STACK_SIZE];
static unsigned long long *b_sp;
static unsigned long long *b_tos;
static unsigned long long *b_bos;

static unsigned long long logical_stack[LOGICAL_STACK_SIZE];
static unsigned long long *l_sp;
static unsigned long long *l_tos;
static unsigned long long *l_bos;

static unsigned long token_args(dbg_regs *dbgframe,
				unsigned long type)
{
	if (!dbgframe)
		return 0;

	switch (type) {
	case PF_TOKEN:
		return dbgframe->t_flags & PF_FLAG;
	case ZF_TOKEN:
		return dbgframe->t_flags & ZF_FLAG;
	case IF_TOKEN:
		return dbgframe->t_flags & IF_FLAG;
	case OF_TOKEN:
		return dbgframe->t_flags & OF_FLAG;
	case DF_TOKEN:
		return dbgframe->t_flags & DF_FLAG;
	case VM_TOKEN:
		return dbgframe->t_flags & VM_FLAG;
	case AF_TOKEN:
		return dbgframe->t_flags & AF_FLAG;
	case AC_TOKEN:
		return dbgframe->t_flags & AC_FLAG;
	case SF_TOKEN:
		return dbgframe->t_flags & SF_FLAG;
	case CF_TOKEN:
		return dbgframe->t_flags & CF_FLAG;

	case CS_ADDR_TOKEN:
	case CS_TOKEN:
		return dbgframe->t_cs;
	case DS_ADDR_TOKEN:
	case DS_TOKEN:
		return dbgframe->t_ds;
	case ES_ADDR_TOKEN:
	case ES_TOKEN:
		return dbgframe->t_es;
	case FS_ADDR_TOKEN:
	case FS_TOKEN:
		return dbgframe->t_fs;
	case GS_ADDR_TOKEN:
	case GS_TOKEN:
		return dbgframe->t_gs;
	case SS_ADDR_TOKEN:
	case SS_TOKEN:
		return dbgframe->t_ss;

	case AL_TOKEN:
		return dbgframe->t_ax & 0xFF;
	case BL_TOKEN:
		return dbgframe->t_bx & 0xFF;
	case CL_TOKEN:
		return dbgframe->t_cx & 0xFF;
	case DL_TOKEN:
		return dbgframe->t_dx & 0xFF;

	case AX_TOKEN:
		return dbgframe->t_ax & 0xFFFF;
	case BX_TOKEN:
		return dbgframe->t_bx & 0xFFFF;
	case CX_TOKEN:
		return dbgframe->t_cx & 0xFFFF;
	case DX_TOKEN:
		return dbgframe->t_dx & 0xFFFF;
	case SI_TOKEN:
		return dbgframe->t_si & 0xFFFF;
	case DI_TOKEN:
		return dbgframe->t_di & 0xFFFF;
	case BP_TOKEN:
		return dbgframe->t_bp & 0xFFFF;
	case SP_TOKEN:
		return dbgframe->t_sp & 0xFFFF;

#if IS_ENABLED(CONFIG_X86_64)
	case RAX_TOKEN:
		return dbgframe->t_ax;
	case RBX_TOKEN:
		return dbgframe->t_bx;
	case RCX_TOKEN:
		return dbgframe->t_cx;
	case RDX_TOKEN:
		return dbgframe->t_dx;
	case RSI_TOKEN:
		return dbgframe->t_si;
	case RDI_TOKEN:
		return dbgframe->t_di;
	case RBP_TOKEN:
		return dbgframe->t_bp;
	case RSP_TOKEN:
		return dbgframe->t_sp;

	case EAX_TOKEN:
		return dbgframe->t_ax & 0xFFFFFFFF;
	case EBX_TOKEN:
		return dbgframe->t_bx & 0xFFFFFFFF;
	case ECX_TOKEN:
		return dbgframe->t_cx & 0xFFFFFFFF;
	case EDX_TOKEN:
		return dbgframe->t_dx & 0xFFFFFFFF;
	case ESI_TOKEN:
		return dbgframe->t_si & 0xFFFFFFFF;
	case EDI_TOKEN:
		return dbgframe->t_di & 0xFFFFFFFF;
	case EBP_TOKEN:
		return dbgframe->t_bp & 0xFFFFFFFF;
	case ESP_TOKEN:
		return dbgframe->t_sp & 0xFFFFFFFF;

	case R8_TOKEN:
		return dbgframe->r8;
	case R9_TOKEN:
		return dbgframe->r9;
	case R10_TOKEN:
		return dbgframe->r10;
	case R11_TOKEN:
		return dbgframe->r11;
	case R12_TOKEN:
		return dbgframe->r12;
	case R13_TOKEN:
		return dbgframe->r13;
	case R14_TOKEN:
		return dbgframe->r14;
	case R15_TOKEN:
		return dbgframe->r15;
#else
	case EAX_TOKEN:
		return dbgframe->t_ax;
	case EBX_TOKEN:
		return dbgframe->t_bx;
	case ECX_TOKEN:
		return dbgframe->t_cx;
	case EDX_TOKEN:
		return dbgframe->t_dx;
	case ESI_TOKEN:
		return dbgframe->t_si;
	case EDI_TOKEN:
		return dbgframe->t_di;
	case EBP_TOKEN:
		return dbgframe->t_bp;
	case ESP_TOKEN:
		return dbgframe->t_sp;
#endif
	case IP_TOKEN:
		return dbgframe->t_ip;

	default:
		break;
	}
	return 0;
}

static inline unsigned long long get_number(unsigned char *p,
					    unsigned char **rp,
					    unsigned long *opl,
					    unsigned long *ret_code,
					    unsigned long mode)
{
	unsigned char *op, *pp = NULL;
	unsigned long long c = 0;
	unsigned long decimal = 0, hex_found = 0, invalid = 0, valid = 0;

	if (rp)
		*rp = p;
	op = p;
	pp = p;

	if (!strncasecmp(p, "0x", 2)) {
		hex_found = 1;
		p++;
		p++;
		pp = p;
	} else
		if (*p == 'X' || *p == 'x') {
			hex_found = 1;
			p++;
			pp = p;
		}

	while (*p) {
		if (delim_table[((*p) & 0x0FF)])
			break;

		if (*p >= '0' && *p <= '9') {
			valid++;
			p++;
		} else if (*p >= 'A' && *p <= 'F') {
			hex_found = 1;
			valid++;
			p++;
		} else if (*p >= 'a' && *p <= 'f') {
			hex_found = 1;
			valid++;
			p++;
		} else {
			invalid = 1;
			break;
		}
	}

	if (invalid) {
		/* invalid string */
		if (ret_code)
			*ret_code = -1;
		return 0;
	}

	if (rp)
		*rp = p;
	if (opl)
		*opl = p - op;

	p = pp;

	if (mode)
		decimal = 1;

	if (hex_found) {
		/* parse as hex number */
		while (*p) {
			if (*p >= '0' && *p <= '9')
				c = (c << 4) | (*p - '0');
			else if (*p >= 'A' && *p <= 'F')
				c = (c << 4) | (*p - 'A' + 10);
			else if (*p >= 'a' && *p <= 'f')
				c = (c << 4) | (*p - 'a' + 10);
			else
				break;
			p++;
		}
	} else
		if (decimal) {
			/* parse as decimal number */
			while (*p) {
				if (*p >= '0' && *p <= '9')
					c = (c * 10) + (*p - '0');
				else
					break;
				p++;
			}
		} else  /* default parses as decimal */ {
			/* parse as decimal number */
			while (*p) {
				if (*p >= '0' && *p <= '9')
					c = (c * 10) + (*p - '0');
				else
					break;
				p++;
			}
		}

	if (ret_code)
		*ret_code = 0;

	return c;
}

unsigned long get_value_from_token(unsigned char *symbol, dbg_regs *dbgframe,
				   unsigned long *ret_code, unsigned long *type)
{
	register int i, len;

	if (ret_code)
		*ret_code = -1;

	if (!dbgframe)
		return 0;

	len = strlen(symbol);
	for (i = 0; len && i < ARRAY_SIZE(dbgframe_symbols);
	     i++) {
		if ((len == dbgframe_symbols[i].symbol_len) &&
		    (!strcasecmp(symbol, dbgframe_symbols[i].symbol))) {
			if (type)
				*type = dbgframe_symbols[i].type;
			if (ret_code)
				*ret_code = 0;
			return token_args(dbgframe,
					  dbgframe_symbols[i].type);
		}
	}
	return 0;
}

static inline void set_token(unsigned long value,
			     unsigned long type,
			     unsigned long index)
{
	token_index[token_count] = index;
	token_value[token_count] = value;
	token_type[token_count++] = type;
}

static inline unsigned char *parse_tokens(dbg_regs *dbgframe,
					  unsigned char *p,
					  unsigned long mode)
{
	register unsigned long i, value;
	unsigned char symbol[256];
	unsigned char *op, *prev, *next;
	unsigned long delta, ret_code = 0, type;

	op = p;
	token_count = 0;
	while (token_count < 255 && p - op < 255) {
		for (prev = p, i = 0; i < 255; i++) {
			if (delim_table[((p[i]) & 0xFF)])
				break;
			if (!isprint(p[i]))
				break;
			symbol[i] = p[i];
		}
		symbol[i] = '\0';
		next = &p[i];

		if (*symbol) {
			value = get_value_from_token(symbol, dbgframe,
						     &ret_code, &type);
			if (ret_code) {
				value = get_number(symbol, NULL, &delta,
						   &ret_code, mode);
				if (ret_code) {
					value = get_value_from_symbol(symbol);
					if (!value) {
						set_token(value,
							  INVALID_NUMBER_TOKEN,
							  p - op);
					} else {
						set_token(value, NUMBER_TOKEN,
							  p - op);
					}
				} else {
					set_token(value, NUMBER_TOKEN, p - op);
				}
			} else {
				set_token(value, type, p - op);
			}
			p = next;
		}

		if (delim_table[*p & 0x0FF]) {
			switch (*p) {
			case '\0':
				set_token(0, NULL_TOKEN, p - op);
				return p;

			case ']':
				set_token(0, DREF_CLOSE_TOKEN, p - op);
				p++;
				break;

			case '(':
				set_token(0, BB_TOKEN, p - op);
				p++;
				break;

			case ')':
				set_token(0, EB_TOKEN, p - op);
				p++;
				break;

			case '+':
				set_token(0, PLUS_TOKEN, p - op);
				p++;
				break;

			case '-':
				set_token(0, MINUS_TOKEN, p - op);
				p++;
				break;

			case '*':
				set_token(0, MULTIPLY_TOKEN, p - op);
				p++;
				break;

			case '/':
				set_token(0, DIVIDE_TOKEN, p - op);
				p++;
				break;

			case '%':
				set_token(0, MOD_TOKEN, p - op);
				p++;
				break;

			case '~':
				set_token(0, NEG_TOKEN, p - op);
				p++;
				break;

			case '^':
				set_token(0, XOR_TOKEN, p - op);
				p++;
				break;

			case '!':
				p++;
				if (*p == '=') {
					set_token(0, NOT_EQUAL_TOKEN, p - op);
					p++;
					break;
				}
				set_token(0, NOT_TOKEN, p - op);
				break;

			case ' ':   /* drop spaces on the floor */
				p++;
				break;

			case '[':
				set_token(0, DREF_OPEN_TOKEN, p - op);
				p++;
				if (tolower(*p) == 'q' && *(p + 1) == ' ') {
					set_token(0, QWORD_TOKEN, p - op);
					p++;
					break;
				}
				if (tolower(*p) == 'd' && *(p + 1) == ' ') {
					set_token(0, DWORD_TOKEN, p - op);
					p++;
					break;
				}
				if (tolower(*p) == 'w' && *(p + 1) == ' ') {
					set_token(0, WORD_TOKEN, p - op);
					p++;
					break;
				}
				if (tolower(*p) == 'b' && *(p + 1) == ' ') {
					set_token(0, BYTE_TOKEN, p - op);
					p++;
					break;
				}
				break;

			case '=':
				p++;
				if (*p == '=') {
					set_token(0, EQUAL_TOKEN, p - op);
					p++;
					break;
				}
				set_token(0, ASSIGNMENT_TOKEN, p - op);
				break;

			case '<':
				p++;
				if (*p == '<') {
					set_token(0, LEFT_SHIFT_TOKEN,
						  p - op);
					p++;
					break;
				}
				if (*p == '=') {
					set_token(0, LESS_EQUAL_TOKEN,
						  p - op);
					p++;
					break;
				}
				set_token(0, LESS_TOKEN, p - op);
				break;

			case '>':
				p++;
				if (*p == '>') {
					set_token(0, RIGHT_SHIFT_TOKEN,
						  p - op);
					p++;
					break;
				}
				if (*p == '=') {
					set_token(0, GREATER_EQUAL_TOKEN,
						  p - op);
					p++;
					break;
				}
				set_token(0, GREATER_TOKEN, p - op);
				break;

			case '|':
				p++;
				if (*p == '|') {
					set_token(0, LOGICAL_OR_TOKEN,
						  p - op);
					p++;
					break;
				}
				set_token(0, OR_TOKEN, p - op);
				break;

			case '&':
				p++;
				if (*p == '&') {
					set_token(0, LOGICAL_AND_TOKEN,
						  p - op);
					p++;
					break;
				}
				set_token(0, AND_TOKEN, p - op);
				break;

			default:
				break;
			}
		}

		/* if the string did not advance, bump the pointer. */
		if (*p && prev >= p)
			p++;

	}  /* while */
	return p;
}

void display_expr_help(void)
{
#if IS_ENABLED(CONFIG_X86_64)
	dbg_pr("Arithmetic Operators\n");
	dbg_pr("\n");
	dbg_pr("+   add\n");
	dbg_pr("-   subtract\n");
	dbg_pr("*   multiply\n");
	dbg_pr("/   divide\n");
	dbg_pr("<<  bit shift left\n");
	dbg_pr(">>  bit shift right\n");
	dbg_pr("|   OR operator\n");
	dbg_pr("&   AND operator\n");
	dbg_pr("^   XOR operator\n");
	dbg_pr("~   NEG operator\n");
	dbg_pr("%%   MODULO operator\n");
	dbg_pr("\n");
	dbg_pr("Example 1:\n");
	dbg_pr("(0)> .e (0x100 + 0x100)\n");
	dbg_pr("(0)> result = 0x200 (512)\n");
	dbg_pr("\n");
	dbg_pr("Example 2:\n");
	dbg_pr("(0)> .e (1 << 20)\n");
	dbg_pr("(0)> result = 0x00100000 (1,024,000)\n");
	dbg_pr("\n");
	dbg_pr("Example 3:\n");
	dbg_pr("(0)> .e (0xFEF023 & 0x100F)\n");
	dbg_pr("(0)> result = 0x1003 (4099)\n");
	dbg_pr("\n");
	dbg_pr("Boolean Operators (Conditional Breakpoint)\n");
	dbg_pr("\n");
	dbg_pr("==      is equal to\n");
	dbg_pr("!=      is not equal to\n");
	dbg_pr("!<expr> is not\n");
	dbg_pr(">       is greater than\n");
	dbg_pr("<       is less than\n");
	dbg_pr(">=      is greater than or equal to\n");
	dbg_pr("<=      if less than or equal to\n");
	dbg_pr("||      logical OR operator\n");
	dbg_pr("&&      logical AND operator\n");
	dbg_pr("\n");
	dbg_pr("all breakpoint conditions must be enclosed in ");
	dbg_pr("brackets () to evaluate correctly\n");
	dbg_pr("\n");
	dbg_pr("Example 1 (Execute Breakpoint):\n");
	dbg_pr("(0)> b 0x37000 (RAX == 20 && RBX <= 4000)\n");
	dbg_pr("breakpoint will activate if condition is true (1)\n");
	dbg_pr("\n");
	dbg_pr("Example 2 (RW Breakpoint):\n");
	dbg_pr("(0)> br 0x3D4 (!RBX &&[d RSI+40] != 2000)\n");
	dbg_pr("breakpoint will activate if condition is true (1)\n");
	dbg_pr("\n");
	dbg_pr("\n");
	dbg_pr("Register Operators\n");
	dbg_pr("\n");
	dbg_pr("RAX, RBX, RCX, RDX        - general registers\n");
	dbg_pr(" R8,  R9, R10, R11        - general registers\n");
	dbg_pr("R12, R13, R14, R15        - general registers\n");
	dbg_pr("RSI, RDI, RBP, RSP        - pointer registers\n");
	dbg_pr("RIP, <symbol>             - IP pointer or symbol\n");
	dbg_pr("CS, DS, ES, FS, GS, SS    - segment registers\n");
	dbg_pr("CF, PF, AF, ZF, SF, IF    - flags\n");
	dbg_pr("DF, OF, VM, AC\n");
	dbg_pr("=                         - set equal to\n");
	dbg_pr("\n");
	dbg_pr("Example 1:\n");
	dbg_pr("(0)> RAX = 0x0032700\n");
	dbg_pr("RAX changed to 0x0032700\n");
	dbg_pr("\n");
	dbg_pr("Example 2:\n");
	dbg_pr("(0)> u thread_switch\n");
	dbg_pr("unassembles function thread_switch\n");
	dbg_pr("\n");
	dbg_pr("Example 3 (Dump):\n");
	dbg_pr("(0)> d RBP+RCX\n");
	dbg_pr("(dumps[RBP + RCX])\n");
	dbg_pr("[addr] 00 00 00 01 02 04 07 ...\n");
	dbg_pr("[addr] 00 34 56 00 7A 01 00 ...\n");
	dbg_pr("\n");
	dbg_pr("Bracket Operators\n");
	dbg_pr("\n");
	dbg_pr("(begin expression bracket\n");
	dbg_pr(")       end expression bracket\n");
	dbg_pr("[       begin pointer\n");
	dbg_pr("]       end pointer\n");
	dbg_pr("q       QWORD reference\n");
	dbg_pr("d       DWORD reference\n");
	dbg_pr("w       WORD reference\n");
	dbg_pr("b       BYTE reference\n");
	dbg_pr("QWORD, DWORD, WORD, and BYTE dereference operators\n");
	dbg_pr("immediately follow pointer brackets\n");
	dbg_pr("i.e.[d <addr/symbol>] or[w <addr/symbol>] or\n");
	dbg_pr("[b <addr/symbol>], etc.\n");
	dbg_pr("\n");
	dbg_pr("Example 1 (dump):\n");
	dbg_pr("(0)> d[d RAX+100]\n");
	dbg_pr("[rax + 100 (dec)] 00 00 00 01 02 04 07 00\n");
	dbg_pr("\n");
	dbg_pr("Example 2 (dump):\n");
	dbg_pr("(0)> d[w 0x003400]\n");
	dbg_pr("[addr (hex)] 00 22 00 01 02 04 07 ...\n");
	dbg_pr("[addr (hex)] 00 31 A1 00 6A 05 00 ...\n");
	dbg_pr("\n");
	dbg_pr("Example 3 (break):\n");
	dbg_pr("(0)> b 0x7A000 (RAX==30) && ([d 0xB8000+50]==0x07)\n");
	dbg_pr("breakpoint will activate if condition is true\n");
	dbg_pr("(returns 1)\n");
#else
	dbg_pr("Arithmetic Operators\n");
	dbg_pr("\n");
	dbg_pr("+   add\n");
	dbg_pr("-   subtract\n");
	dbg_pr("*   multiply\n");
	dbg_pr("/   divide\n");
	dbg_pr("<<  bit shift left\n");
	dbg_pr(">>  bit shift right\n");
	dbg_pr("|   OR operator\n");
	dbg_pr("&   AND operator\n");
	dbg_pr("^   XOR operator\n");
	dbg_pr("~   NEG operator\n");
	dbg_pr("%%   MODULO operator\n");
	dbg_pr("\n");
	dbg_pr("Example 1:\n");
	dbg_pr("(0)> .e (0x100 + 0x100)\n");
	dbg_pr("(0)> result = 0x200 (512)\n");
	dbg_pr("\n");
	dbg_pr("Example 2:\n");
	dbg_pr("(0)> .e (1 << 20)\n");
	dbg_pr("(0)> result = 0x00100000 (1,024,000)\n");
	dbg_pr("\n");
	dbg_pr("Example 3:\n");
	dbg_pr("(0)> .e (0xFEF023 & 0x100F)\n");
	dbg_pr("(0)> result = 0x1003 (4099)\n");
	dbg_pr("\n");
	dbg_pr("Boolean Operators (Conditional Breakpoint)\n");
	dbg_pr("\n");
	dbg_pr("==      is equal to\n");
	dbg_pr("!=      is not equal to\n");
	dbg_pr("!<expr> is not\n");
	dbg_pr(">       is greater than\n");
	dbg_pr("<       is less than\n");
	dbg_pr(">=      is greater than or equal to\n");
	dbg_pr("<=      if less than or equal to\n");
	dbg_pr("||      logical OR operator\n");
	dbg_pr("&&      logical AND operator\n");
	dbg_pr("\n");
	dbg_pr("all breakpoint conditions must be enclosed in ");
	dbg_pr("brackets () to evaluate correctly\n");
	dbg_pr("\n");
	dbg_pr("Example 1 (Execute Breakpoint):\n");
	dbg_pr("(0)> b 0x37000 (RAX == 20 && RBX <= 4000)\n");
	dbg_pr("breakpoint will activate if condition is true (1)\n");
	dbg_pr("\n");
	dbg_pr("Example 2 (RW Breakpoint):\n");
	dbg_pr("(0)> br 0x3D4 (!RBX &&[d RSI+40] != 2000)\n");
	dbg_pr("breakpoint will activate if condition is true (1)\n");
	dbg_pr("\n");
	dbg_pr("\n");
	dbg_pr("Register Operators\n");
	dbg_pr("\n");
	dbg_pr("EAX, EBX, ECX, EDX        - general registers\n");
	dbg_pr("ESI, EDI, EBP, ESP        - pointer registers\n");
	dbg_pr("EIP, <symbol>             - IP pointer or symbol\n");
	dbg_pr("CS, DS, ES, FS, GS, SS    - segment registers\n");
	dbg_pr("CF, PF, AF, ZF, SF, IF    - flags\n");
	dbg_pr("DF, OF, VM, AC\n");
	dbg_pr("=                         - set equal to\n");
	dbg_pr("\n");
	dbg_pr("Example 1:\n");
	dbg_pr("(0)> EAX = 0x0032700\n");
	dbg_pr("EAX changed to 0x0032700\n");
	dbg_pr("\n");
	dbg_pr("Example 2:\n");
	dbg_pr("(0)> u thread_switch\n");
	dbg_pr("unassembles function thread_switch\n");
	dbg_pr("\n");
	dbg_pr("Example 3 (Dump):\n");
	dbg_pr("(0)> d EBP+ECX\n");
	dbg_pr("(dumps[d EBP + ECX])\n");
	dbg_pr("[addr] 00 00 00 01 02 04 07 ...\n");
	dbg_pr("[addr] 00 34 56 00 7A 01 00 ...\n");
	dbg_pr("\n");
	dbg_pr("Bracket Operators\n");
	dbg_pr("\n");
	dbg_pr("(begin expression bracket\n");
	dbg_pr(")       end expression bracket\n");
	dbg_pr("[       begin pointer\n");
	dbg_pr("]       end pointer\n");
	dbg_pr("d       DWORD reference\n");
	dbg_pr("w       WORD reference\n");
	dbg_pr("b       BYTE reference\n");
	dbg_pr("DWORD, WORD, and BYTE dereference operators\n");
	dbg_pr("immediately follow pointer brackets\n");
	dbg_pr("i.e.[d <addr/symbol>] or[w <addr/symbol>] or\n");
	dbg_pr("[b <addr/symbol>], etc.\n");
	dbg_pr("\n");
	dbg_pr("Example 1 (dump):\n");
	dbg_pr("(0)> d[d EAX+100]\n");
	dbg_pr("[eax + 100 (dec)] 00 00 00 01 02 04 07 00\n");
	dbg_pr("\n");
	dbg_pr("Example 2 (dump):\n");
	dbg_pr("(0)> d[w 0x003400]\n");
	dbg_pr("[addr (hex)] 00 22 00 01 02 04 07 ...\n");
	dbg_pr("[addr (hex)] 00 31 A1 00 6A 05 00 ...\n");
	dbg_pr("\n");
	dbg_pr("Example 3 (break):\n");
	dbg_pr("(0)> b 0x7A000 (RAX==30) && ([d 0xB8000+50]==0x07)\n");
	dbg_pr("breakpoint will activate if condition is true\n");
	dbg_pr("(returns 1)\n");
	dbg_pr("\n");
#endif
}

static inline u64 deref(unsigned long type,
			unsigned long value,
			int sizeflag,
			unsigned char **result,
			unsigned long seg,
			unsigned long sv)
{
	u64 *pq;

	/* if a sizeflag was specified, override the type field */
	if (sizeflag) {
		switch (sizeflag) {
			/* BYTE */
		case 1:
			return (unsigned char)mdb_segment_getword(sv,
								  value, 1);

			/* WORD */
		case 2:
			return (unsigned short)mdb_segment_getword(sv,
								   value, 2);

			/* DWORD */
		case 4:
			return (unsigned long)mdb_segment_getword(sv,
								  value, 4);

			/* QWORD */
		case 8:
			pq = (u64 *)value;
			return (u64)mdb_segment_getqword(sv,
							 (u64 *)pq,
							 8);

			/* FWORD */
		case 6:
			/* TBYTE (floating point) */
		case 10:
			/* VMMWORD */
		case 16:
			if (result)
				*result = (unsigned char *)value;
			return 0;

			/* if an unknown sizeflag is passed,
			 * then default to type
			 */
		default:
			break;
		}
	}

	switch (type) {
	case ARCH_PTR:
#if IS_ENABLED(CONFIG_X86_64)
		pq = (u64 *)value;
		return (u64)mdb_segment_getqword(sv, (u64 *)pq, 8);
#else
		return (unsigned long)mdb_segment_getword(sv, value, 4);
#endif
	case ULONGLONG_PTR:
		pq = (u64 *)value;
		return (u64)mdb_segment_getqword(sv, (u64 *)pq, 8);

	case ULONG_PTR:
		return (unsigned long)mdb_segment_getword(sv, value, 4);

	case WORD_PTR:
		return (unsigned short)mdb_segment_getword(sv, value, 2);

	case BYTE_PTR:
		return (unsigned char)mdb_segment_getword(sv, value, 1);

	default:
		return 0;
	}
}

static inline unsigned long segment_push(unsigned long i)
{
	if (s_sp > s_bos)
		return 0;
	*s_sp = i;
	s_sp++;
	return 1;
}

static inline unsigned long segment_pop(void)
{
	s_sp--;
	if (s_sp < s_tos) {
		s_sp++;
		return -1;
	}
	return *s_sp;
}

static inline unsigned long express_push(unsigned long long i)
{
	if (sp > bos)
		return 0;
	*sp = i;
	sp++;
	return 1;
}

static inline unsigned long long express_pop(void)
{
	sp--;
	if (sp < tos) {
		sp++;
		return 0;
	}
	return *sp;
}

static inline unsigned long context_push(unsigned long long i)
{
	if (c_sp > c_bos)
		return 0;
	*c_sp = i;
	c_sp++;
	return 1;
}

static inline unsigned long long context_pop(void)
{
	c_sp--;
	if (c_sp < c_tos) {
		c_sp++;
		return 0;
	}
	return *c_sp;
}

static inline unsigned long boolean_push(unsigned long long i)
{
	if (b_sp > b_bos)
		return 0;
	*b_sp = i;
	b_sp++;
	return 1;
}

static inline unsigned long long boolean_pop(void)
{
	b_sp--;
	if (b_sp < b_tos) {
		b_sp++;
		return 0;
	}
	return *b_sp;
}

static inline unsigned long logical_push(unsigned long long i)
{
	if (l_sp > l_bos)
		return 0;
	*l_sp = i;
	l_sp++;
	return 1;
}

static inline unsigned long long logical_pop(void)
{
	l_sp--;
	if (l_sp < l_tos) {
		l_sp++;
		return 0;
	}
	return *l_sp;
}

static inline void init_numeric_stacks(void)
{
	sp = num_stack;
	tos = sp;
	bos = sp + NUM_STACK_SIZE - 1;

	s_sp = seg_stack;
	s_tos = s_sp;
	s_bos = s_sp + SEG_STACK_SIZE - 1;

	c_sp = context_stack;
	c_tos = c_sp;
	c_bos = c_sp + CONTEXT_STACK_SIZE - 1;

	b_sp = boolean_stack;
	b_tos = b_sp;
	b_bos = b_sp + BOOL_STACK_SIZE - 1;

	l_sp = logical_stack;
	l_tos = l_sp;
	l_bos = l_sp + LOGICAL_STACK_SIZE - 1;
}

static inline unsigned long process_operator(unsigned long oper)
{
	unsigned long a, b;

	b = express_pop();
	a = express_pop();
	switch (oper) {
	case NEG_TOKEN:
		break;

	case LEFT_SHIFT_TOKEN:
		/* mod (b) to base */
		express_push(a << (b % PROCESSOR_WIDTH));
		break;

	case RIGHT_SHIFT_TOKEN:
		/* mod (b) to base */
		express_push(a >> (b % PROCESSOR_WIDTH));
		break;

	case PLUS_TOKEN:
		express_push(a + b);
		break;

	case XOR_TOKEN:
		express_push(a ^ b);
		break;

	case AND_TOKEN:
		express_push(a & b);
		break;

	case MOD_TOKEN:
		/* if modulo by zero, drop value on the floor */
		if (b)
			express_push(a % b);
		else
			express_push(0);
		break;

	case OR_TOKEN:
		express_push(a | b);
		break;

	case MINUS_TOKEN:
		express_push(a - b);
		break;

	case MULTIPLY_TOKEN:
		express_push(a * b);
		break;

	case DIVIDE_TOKEN:
		/* if divide by zero, drop value on the floor */
		if (b)
			express_push(a / b);
		else
			express_push(0);
		break;
	}
	return 0;
}

static inline unsigned long processboolean(unsigned long oper)
{
	unsigned long a, b;

	b = express_pop();
	a = express_pop();
	switch (oper) {
	case NOT_TOKEN:
		/* we pushed an imaginary zero on the stack */
		/* this operation returns the boolean for (!x) */
		express_push(a == b);
		break;

	case GREATER_TOKEN:
		express_push(a > b);
		break;

	case LESS_TOKEN:
		express_push(a < b);
		break;

	case GREATER_EQUAL_TOKEN:
		express_push(a >= b);
		break;

	case LESS_EQUAL_TOKEN:
		express_push(a <= b);
		break;

	case EQUAL_TOKEN:
		express_push(a == b);
		break;

	case NOT_EQUAL_TOKEN:
		express_push(a != b);
		break;
	}
	return 0;
}

static inline unsigned long process_logical(unsigned long oper)
{
	unsigned long a, b;

	b = express_pop();
	a = express_pop();
	switch (oper) {
	case LOGICAL_AND_TOKEN:
		express_push(a && b);
		break;

	case LOGICAL_OR_TOKEN:
		express_push(a || b);
		break;
	}
	return 0;
}

static inline unsigned long parse_logical(unsigned long logical_count)
{
	register int i, r;
	unsigned long a;
	unsigned long c = 0, last_class = 0, oper = 0;

	for (i = 0; i < logical_count; i++)
		express_push(logical_pop());

	for (i = 0, r = 0; i < (logical_count / 2); i++) {
		a = express_pop();
		token_type[r] = NUMBER_TOKEN;
		token_value[r++] = a;
		a = express_pop();
		/* get the operator type */
		token_type[r] = a;
		token_value[r++] = 0;
	}

	init_numeric_stacks();

	for (i = 0; i < logical_count; i++) {
		switch (token_type[i]) {
		case LOGICAL_AND_TOKEN:
		case LOGICAL_OR_TOKEN:
			if (last_class != CLASS_BOOLEAN) {
				last_class = CLASS_BOOLEAN;
				oper = token_type[i];
			}
			continue;

		case NUMBER_TOKEN:
			if (last_class == CLASS_DATA) {
				c = express_pop();
				return c;
			}
			last_class = CLASS_DATA;
			c = token_value[i];
			express_push(c);
			if (oper)
				oper = process_logical(oper);
			continue;

		case NULL_TOKEN:
			c = express_pop();
			return c;

		default:
			continue;
		}
	}
	return c;
}

static inline unsigned long parse_boolean(unsigned long boolean_count)
{
	register int i, r;
	unsigned long a, oper = 0;
	unsigned long c = 0, last_class = 0, logical_count = 0;

	for (i = 0; i < boolean_count; i++)
		express_push(boolean_pop());

	for (i = 0, r = 0; i < (boolean_count / 2); i++) {
		a = express_pop();
		token_type[r] = NUMBER_TOKEN;
		token_value[r++] = a;
		a = express_pop();
		/* get the operator type */
		token_type[r] = a;
		token_value[r++] = 0;
	}

	init_numeric_stacks();

	for (i = 0; i < boolean_count; i++) {
		switch (token_type[i]) {
			/* partition operators */
		case LOGICAL_AND_TOKEN:
		case LOGICAL_OR_TOKEN:
			c = express_pop();
			logical_push(c);
			logical_count++;
			logical_push(token_type[i]);
			logical_count++;
			express_push(c);
			oper = 0;
			last_class = 0;
			continue;

			/* boolean operators */
		case NOT_TOKEN:
			if (last_class != CLASS_BOOLEAN) {
				express_push(0);
				last_class = CLASS_BOOLEAN;
				oper = token_type[i];
			}
			continue;

		case GREATER_TOKEN:
		case LESS_TOKEN:
		case GREATER_EQUAL_TOKEN:
		case LESS_EQUAL_TOKEN:
		case EQUAL_TOKEN:
		case NOT_EQUAL_TOKEN:
			if (last_class != CLASS_BOOLEAN) {
				last_class = CLASS_BOOLEAN;
				oper = token_type[i];
			}
			continue;

		case NUMBER_TOKEN:
			if (last_class == CLASS_DATA) {
				c = express_pop();
				if (logical_count) {
					logical_push(c);
					logical_count++;
					/* push null token */
					logical_push(0);
					logical_count++;
					c = parse_logical(logical_count);
					return c;
				}
				return c;
			}
			last_class = CLASS_DATA;
			c = token_value[i];
			express_push(c);
			if (oper)
				oper = processboolean(oper);
			continue;

		case NULL_TOKEN:
			c = express_pop();
			if (logical_count) {
				logical_push(c);
				logical_count++;
				/* push null token */
				logical_push(0);
				logical_count++;
				c = parse_logical(logical_count);
				return c;
			}
			return c;

		default:
			continue;
		}
	}
	return c;
}

u64 eeval(dbg_regs *dbgframe,
	  unsigned char **p,
	  unsigned long *type,
	  unsigned long mode,
	  int sizeflag,
	  unsigned char **result)
{
	register int i;
	unsigned long oper = 0, dref = 0, bracket = 0;
	unsigned long dref_type = ARCH_PTR, last_class = 0, last_token = 0;
	unsigned long neg_flag = 0, negative_flag = 0;
	u64 c;
	unsigned long boolean_count = 0, segment = -1, segment_count = 0,
		      segment_value = -1;

	if (type)
		*type = INVALID;
	parse_tokens(dbgframe, *p, mode);
	if (!token_count)
		return 0;

	init_numeric_stacks();
	for (i = 0; i < token_count; i++) {
		switch (token_type[i]) {
		case INVALID_NUMBER_TOKEN:
			goto evaluate_error_exit;

		case NOT_TOKEN:
			if (last_class != CLASS_DATA) {
				if (oper)
					oper = process_operator(oper);
				c = express_pop();
				boolean_push(c);
				boolean_count++;
				boolean_push(token_type[i]);
				boolean_count++;
				dref_type = ARCH_PTR;
				last_class = 0;
				neg_flag  = 0;
				negative_flag = 0;
			}
			last_token = NOT_TOKEN;
			continue;

			/* assignment operators */
		case ASSIGNMENT_TOKEN:
			if (last_class == CLASS_DATA) {
				express_pop();
				dref_type = ARCH_PTR;
				last_class = 0;
				neg_flag  = 0;
				negative_flag = 0;
			}
			last_token = 0;
			continue;

			/* boolean operators */
		case GREATER_TOKEN:
		case LESS_TOKEN:
		case GREATER_EQUAL_TOKEN:
		case LESS_EQUAL_TOKEN:
		case LOGICAL_AND_TOKEN:
		case LOGICAL_OR_TOKEN:
		case EQUAL_TOKEN:
		case NOT_EQUAL_TOKEN:
			if (oper)
				oper = process_operator(oper);
			c = express_pop();
			boolean_push(c);
			boolean_count++;
			boolean_push(token_type[i]);
			boolean_count++;
			dref_type = ARCH_PTR;
			last_class = 0;
			neg_flag  = 0;
			negative_flag = 0;
			last_token = 0;
			continue;

			/* partition operators */
		case QWORD_TOKEN:
			if (dref)
				dref_type = ULONGLONG_PTR;
			last_token = 0;
			continue;

		case DWORD_TOKEN:
			if (dref)
				dref_type = ULONG_PTR;
			last_token = 0;
			continue;

		case WORD_TOKEN:
			if (dref)
				dref_type = WORD_PTR;
			last_token = 0;
			continue;

		case BYTE_TOKEN:
			if (dref)
				dref_type = BYTE_PTR;
			last_token = 0;
			continue;

		case DREF_OPEN_TOKEN:
			/* push state and nest for de-reference */
			if (last_class == CLASS_DATA) {
				*p = (unsigned char *)
					((unsigned long)*p +
					 (unsigned long)token_index[i]);
				if (type) {
					if (boolean_count)
						*type = BOOLEAN;
					else
						*type = NUMERIC;
				}
				c = express_pop();
				if (boolean_count) {
					boolean_push(c);
					boolean_count++;
					/* last operator null token */
					boolean_push(0);
					boolean_count++;
					c = parse_boolean(boolean_count);
					return c;
				}
				return c;
			}
			dref++;
			context_push(dref_type);
			context_push(oper);
			context_push(last_class);
			context_push(neg_flag);
			context_push(negative_flag);
			dref_type = ARCH_PTR;
			oper      = 0;
			last_class = 0;
			neg_flag  = 0;
			negative_flag = 0;
			last_token = 0;
			continue;

		case DREF_CLOSE_TOKEN:
			/* pop state, restore, and complete oper */
			if (!dref)
				continue;

			c = deref(dref_type, express_pop(), sizeflag, result,
				  segment, segment_value);
			express_push(c);
			negative_flag  = context_pop();
			neg_flag  = context_pop();
			context_pop();
			oper      = context_pop();
			dref_type = context_pop();
			if (dref)
				dref--;
			last_class = CLASS_DATA;

			c = express_pop();
			if (negative_flag)
				c = 0 - c;
			if (neg_flag)
				c = ~c;
			neg_flag = 0;
			negative_flag = 0;
			express_push(c);

			if (oper)
				oper = process_operator(oper);
			last_token = 0;
			continue;

		case BB_TOKEN:
			if (last_class == CLASS_DATA) {
				*p = (unsigned char *)
					((unsigned long)
					 *p + (unsigned long)token_index[i]);
				if (type) {
					if (boolean_count)
						*type = BOOLEAN;
					else
						*type = NUMERIC;
				}
				c = express_pop();
				if (boolean_count) {
					boolean_push(c);
					boolean_count++;
					/* last operator is the null token */
					boolean_push(0);
					boolean_count++;
					c = parse_boolean(boolean_count);
					return c;
				}
				return c;
			}
			bracket++;
			context_push(oper);
			context_push(last_class);
			context_push(neg_flag);
			context_push(negative_flag);
			oper      = 0;
			last_class = 0;
			neg_flag  = 0;
			negative_flag = 0;
			last_token = 0;
			continue;

		case EB_TOKEN:
			if (!bracket)
				continue;
			negative_flag  = context_pop();
			neg_flag  = context_pop();
			context_pop();
			oper      = context_pop();
			if (bracket)
				bracket--;
			last_class = CLASS_DATA;
			c = express_pop();
			if (negative_flag)
				c = 0 - c;
			if (neg_flag)
				c = ~c;
			neg_flag = 0;
			negative_flag = 0;
			express_push(c);
			if (oper)
				oper = process_operator(oper);
			last_token = 0;
			continue;

			/* arithmetic operators */
		case NEG_TOKEN:
			neg_flag = 1;
			last_token = 0;
			continue;

		case MINUS_TOKEN:
			if (last_class == CLASS_ARITHMETIC) {
				last_token = MINUS_TOKEN;
				negative_flag = 1;
				continue;
			}
			if (last_class != CLASS_ARITHMETIC) {
				last_class = CLASS_ARITHMETIC;
				oper = token_type[i];
			}
			last_token = 0;
			continue;

		case PLUS_TOKEN:
		case LEFT_SHIFT_TOKEN:
		case RIGHT_SHIFT_TOKEN:
		case XOR_TOKEN:
		case AND_TOKEN:
		case MOD_TOKEN:
		case OR_TOKEN:
		case MULTIPLY_TOKEN:
		case DIVIDE_TOKEN:
			if (last_class != CLASS_ARITHMETIC) {
				last_class = CLASS_ARITHMETIC;
				oper = token_type[i];
			}
			last_token = 0;
			continue;

			/* data operators */
		case CF_TOKEN:
		case PF_TOKEN:
		case AF_TOKEN:
		case ZF_TOKEN:
		case SF_TOKEN:
		case IF_TOKEN:
		case DF_TOKEN:
		case OF_TOKEN:
		case VM_TOKEN:
		case AC_TOKEN:
		case IP_TOKEN:
		case FLAGS_TOKEN:
		case AL_TOKEN:
		case BL_TOKEN:
		case CL_TOKEN:
		case DL_TOKEN:
		case AX_TOKEN:
		case BX_TOKEN:
		case CX_TOKEN:
		case DX_TOKEN:
		case SI_TOKEN:
		case DI_TOKEN:
		case BP_TOKEN:
		case SP_TOKEN:
		case R8_TOKEN:
		case R9_TOKEN:
		case R10_TOKEN:
		case R11_TOKEN:
		case R12_TOKEN:
		case R13_TOKEN:
		case R14_TOKEN:
		case R15_TOKEN:
		case NUMBER_TOKEN:
		case EAX_TOKEN:
		case EBX_TOKEN:
		case ECX_TOKEN:
		case EDX_TOKEN:
		case RAX_TOKEN:
		case RBX_TOKEN:
		case RCX_TOKEN:
		case RDX_TOKEN:
		case ESI_TOKEN:
		case EDI_TOKEN:
		case EBP_TOKEN:
		case ESP_TOKEN:
		case RSI_TOKEN:
		case RDI_TOKEN:
		case RBP_TOKEN:
		case RSP_TOKEN:
			/* get last segment associated with this token */
			if (segment_count) {
				segment = segment_pop();
				segment_value = segment_pop();
				segment_count--;
			}

			if (last_class == CLASS_DATA) {
				*p = (unsigned char *)
					((unsigned long)
					 *p + (unsigned long)token_index[i]);
				if (type) {
					if (boolean_count)
						*type = BOOLEAN;
					else
						*type = NUMERIC;
				}
				c = express_pop();
				if (boolean_count) {
					boolean_push(c);
					boolean_count++;
					/* last operator is the null token */
					boolean_push(0);
					boolean_count++;
					c = parse_boolean(boolean_count);
					return c;
				}
				return c;
			}
			last_class = CLASS_DATA;
			c = token_value[i];
			if (negative_flag)
				c = 0 - c;
			if (neg_flag)
				c = ~token_value[i];
			neg_flag = 0;
			negative_flag = 0;
			express_push(c);
			if (oper)
				oper = process_operator(oper);
			last_token = 0;
			continue;

			/* if a token is tagged for derefence, push segment */
		case CS_ADDR_TOKEN:
		case DS_ADDR_TOKEN:
		case ES_ADDR_TOKEN:
		case FS_ADDR_TOKEN:
		case GS_ADDR_TOKEN:
		case SS_ADDR_TOKEN:
			segment_push(token_value[i]);
			segment_push(token_type[i]);
			segment_count++;
			last_token = 0;
			continue;

		case CS_TOKEN:
		case DS_TOKEN:
		case ES_TOKEN:
		case FS_TOKEN:
		case GS_TOKEN:
		case SS_TOKEN:
			if (last_class == CLASS_DATA) {
				*p = (unsigned char *)((unsigned long)
						       *p +
						       (unsigned long)
						       token_index[i]);
				if (type) {
					if (boolean_count)
						*type = BOOLEAN;
					else
						*type = NUMERIC;
				}
				c = express_pop();
				if (boolean_count) {
					boolean_push(c);
					boolean_count++;
					/* last operator is the null token */
					boolean_push(0);
					boolean_count++;
					c = parse_boolean(boolean_count);
					return c;
				}
				return c;
			}
			last_class = CLASS_DATA;
			c = token_value[i];
			if (negative_flag)
				c = 0 - c;
			if (neg_flag)
				c = ~token_value[i];
			neg_flag = 0;
			negative_flag = 0;
			express_push(c);
			if (oper)
				oper = process_operator(oper);
			last_token = 0;
			continue;

		case NULL_TOKEN:
			*p = (unsigned char *)
				((unsigned long)*p +
				 (unsigned long)token_index[i]);
			if (token_count > 1 && type) {
				if (boolean_count)
					*type = BOOLEAN;
				else
					*type = NUMERIC;
			}
			c = express_pop();
			if (boolean_count) {
				boolean_push(c);
				boolean_count++;
				boolean_push(0);
				boolean_count++;
				c = parse_boolean(boolean_count);
				return c;
			}
			return c;

		default:
			last_token = 0;
			continue;
		}
	}

evaluate_error_exit:
	if (type)
		*type = INVALID;

	return 0;
}

u64 eval_disasm_expr(dbg_regs *dbgframe,
		     unsigned char **p,
		     unsigned long *type,
		     int sizeflag,
		     unsigned char **result)
{
	register u64 c;

	if (result)
		*result = NULL;
	c = eeval(dbgframe, p, type, 1, sizeflag, result);
	return c;
}

u64 eval_numeric_expr(dbg_regs *dbgframe,
		      unsigned char **p,
		      unsigned long *type)
{
	register u64 c;

	c = eeval(dbgframe, p, type, 1, 0, NULL);
	return c;
}

u64 eval_expr(dbg_regs *dbgframe,
	      unsigned char **p,
	      unsigned long *type)
{
	register u64 c;

	c = eeval(dbgframe, p, type, 0, 0, NULL);
	return c;
}

void eval_command_expr(dbg_regs *dbgframe,
		       unsigned char *p)
{
	unsigned char *expr;
	unsigned long type;
	u64 c;

	expr = p;
	c = eval_expr(dbgframe, &p, &type);
	if (type) {
		dbg_pr("expr: %s = 0x%llX (%lld) (%s) bool(%i) = %s\n",
		       expr, c, c, expr_description[type & 3],
		       (c) ? 1 : 0, (c) ? "TRUE" : "FALSE");
	} else {
		dbg_pr("expression parameters invalid\n");
		dbg_pr("expr: %s = 0x%llX (%lld) (results invalid) (%s)",
		       expr, c, c, expr_description[type & 3]);
		dbg_pr(" bool(%i) = %s\n",
		       (c) ? 1 : 0, (c) ? "TRUE" : "FALSE");
	}
}
