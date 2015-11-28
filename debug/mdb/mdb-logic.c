
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
#include <linux/smp_lock.h>
#include <linux/ctype.h>
#include <linux/keyboard.h>
#include <linux/console.h>
#include <linux/serial_reg.h>
#include <linux/uaccess.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/atomic.h>
#include <asm/msr.h>
#include <linux/io.h>

#ifdef CONFIG_SMP
#include <mach_apic.h>
#include <mach_ipi.h>
#endif

#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>
#include <linux/kallsyms.h>

#include "mdb.h"
#include "mdb-ia32.h"
#include "mdb-list.h"
#include "mdb-ia32-proc.h"
#include "mdb-base.h"
#include "mdb-proc.h"
#include "mdb-os.h"
#include "mdb-keyboard.h"

#define PROCESSOR_32        32
#define PROCESSOR_64        64

#define DEBUG_EXPRESS        0
#define DEBUG_BOOL           0
#define DEBUG_LOGICAL        0
#define DEBUG_LOGICAL_STACK  0
#define DEBUG_BOOL_STACK     0

#define INVALID_EXPRESSION  0
#define NUMERIC_EXPRESSION  1
#define BOOLEAN_EXPRESSION  2

#define NUM_STACK_SIZE      256
#define CONTEXT_STACK_SIZE  1024
#define BOOL_STACK_SIZE     256
#define LOGICAL_STACK_SIZE  256

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
#define EAX_TOKEN            18
#define EBX_TOKEN            19
#define ECX_TOKEN            20
#define EDX_TOKEN            21
#define ESI_TOKEN            22
#define EDI_TOKEN            23
#define EBP_TOKEN            24
#define ESP_TOKEN            25
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
#define EIP_TOKEN            38
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

#define   ULONG_PTR         0
#define   WORD_PTR          1
#define   BYTE_PTR          2

#define   CLASS_DATA        1
#define   CLASS_ASSIGN      2
#define   CLASS_PARTITION   3
#define   CLASS_ARITHMETIC  4
#define   CLASS_BOOLEAN     5

unsigned char *exprDescription[]={
     "INVALID",
     "NUMERIC",
     "BOOLEAN",
     "???????",
};

unsigned char *parserDescription[]={
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
     "EAX_TOKEN",
     "EBX_TOKEN",
     "ECX_TOKEN",
     "EDX_TOKEN",
     "ESI_TOKEN",
     "EDI_TOKEN",
     "EBP_TOKEN",
     "ESP_TOKEN",
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
     "EIP_TOKEN",
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
     "NOT_EQUAL_TOKEN"
     "INVALID_NUMBER_TOKEN"
};

unsigned char TokenIndex[256];
unsigned char TokenClass[256];
unsigned char TokenType[256];
unsigned long TokenValue[256];
unsigned long TokenCount;

unsigned long numStack[NUM_STACK_SIZE];
unsigned long *sp;
unsigned long *tos;
unsigned long *bos;

unsigned long contextStack[CONTEXT_STACK_SIZE];
unsigned long *c_sp;
unsigned long *c_tos;
unsigned long *c_bos;

unsigned long booleanStack[BOOL_STACK_SIZE];
unsigned long *b_sp;
unsigned long *b_tos;
unsigned long *b_bos;

unsigned long logicalStack[LOGICAL_STACK_SIZE];
unsigned long *l_sp;
unsigned long *l_tos;
unsigned long *l_bos;

extern unsigned long GetValueFromSymbol(unsigned char *symbolName);
extern unsigned char delim_table[256];

#ifdef LINUX_DRIVER
spinlock_t expressLock = SPIN_LOCK_UNLOCKED;
static long flags;
#endif

unsigned long GetNumber(unsigned char *p, unsigned char **rp, unsigned long *opl, unsigned long *retCode, unsigned long mode)
{

    unsigned char *op, *pp = NULL;
    unsigned long c = 0, decimal = 0, hex_found = 0, invalid = 0, valid = 0;

    pp = op = p;
    while (*p)
    {
       if (*p == 'X' || *p == 'x')
       {
          hex_found = 1;
          p++;
          pp = p;
       }

       if (*p >= '0' && *p <= '9')
       {
          valid++;
	  p++;
       }
       else
       if (*p >= 'A' && *p <= 'F')
       {
	  hex_found = 1;
          valid++;
	  p++;
       }
       else
       if (*p >= 'a' && *p <= 'f')
       {
	  hex_found = 1;
          valid++;
	  p++;
       }
       else
       if ((*p == 'R') || (*p == 'r'))
       {
	  decimal = 1;
	  p++;
       }
       else
       if (delim_table[((*p) & 0xFF)])
	  break;
       else
       {
          invalid = 1;
          break;
       }
    }

    if (rp)
       *rp = p;
    if (opl)
       *opl = (unsigned long)((unsigned long)p - (unsigned long) op);

    if (invalid && !valid)
    {
       if (retCode)
          *retCode = -1;   /* invalid string */
       return 0;
    }

    p = pp;

    if (mode)
       decimal = 1;

    if (decimal)
    {
       /* parse as decimal number */
       while (*p)
       {
	     if (*p >= '0' && *p <= '9')
		c = (c * 10) + (*p - '0');
	     else
		break;
	  p++;
       }
    }
    else
    if (hex_found)
    {
       /* parse as hex number */
       while (*p)
       {
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
    }
    else  /* default parses as hex */
    {
       /* parse as hex number */
       while (*p)
       {
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
    }

    if (retCode)
       *retCode = 0;

    return (c);

}


unsigned char *parseTokens(StackFrame *stackFrame, unsigned char *p, unsigned long mode)
{

    register unsigned long i, value;
    unsigned char symbol[256], *s;
    unsigned char *tmp, *op;
    unsigned long delta, retCode = 0;

    op = p;
    TokenCount = 0;
    while (TokenCount < 200 && (unsigned long)p - (unsigned long)op < 200)
    {
       if (isalpha(*p) || *p == '_' ||  *p == '@' || *p == '$')
       {
	  s = p;
	  for (i = 0; i < 255; i++)
          {
             if (delim_table[((s[i]) & 0xFF)])
                break;

             if (!isprint(s[i]))
                break;

	     symbol[i] = s[i];
          }
	  symbol[i] = '\0';

	  value = GetValueFromSymbol(symbol);
	  if (value)
	  {
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = value;
	     TokenType[TokenCount++] = NUMBER_TOKEN;
	     p = &s[i];   /* bump the pointer past the symbol */
	  }
       }

       if (stackFrame)
       {
	 switch (*p)
	 {

	  case '\0':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = NULL_TOKEN;
	     return (p);

	  case ']':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = DREF_CLOSE_TOKEN;
	     p++;
	     break;

	  case '(':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = BB_TOKEN;
	     p++;
	     break;

	  case ')':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = EB_TOKEN;
	     p++;
	     break;

	  case '+':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = PLUS_TOKEN;
	     p++;
	     break;

	  case '-':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = MINUS_TOKEN;
	     p++;
	     break;

	  case '*':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = MULTIPLY_TOKEN;
	     p++;
	     break;

	  case '/':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = DIVIDE_TOKEN;
	     p++;
	     break;

	  case '%':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = MOD_TOKEN;
	     p++;
	     break;

	  case '~':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = NEG_TOKEN;
	     p++;
	     break;

	  case '^':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = XOR_TOKEN;
	     p++;
	     break;

	  case '!':
	     p++;
	     if (*p == '=')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = NOT_EQUAL_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = NOT_TOKEN;
	     break;

	  case ' ':   /* drop spaces on the floor */
	     p++;
	     break;

	  /*
	   *   These cases require special handling
	   */

	  case 'p':
	  case 'P':
	     p++;
	     if (*p == 'F' || *p == 'f')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & PF_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = PF_TOKEN;
		p++;
		break;
	     }
	     break;

	  case 'z':
	  case 'Z':
	     p++;
	     if (*p == 'F' || *p == 'f')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & ZF_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = ZF_TOKEN;
		p++;
		break;
	     }
	     break;

	  case 'i':
	  case 'I':
	     p++;
	     if (*p == 'F' || *p == 'f')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & IF_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = IF_TOKEN;
		p++;
		break;
	     }
	     break;

	  case 'o':
	  case 'O':
	     p++;
	     if (*p == 'F' || *p == 'f')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & OF_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = OF_TOKEN;
		p++;
		break;
	     }
	     break;

	  case 'v':
	  case 'V':
	     p++;
	     if (*p == 'M' || *p == 'm')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & VM_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = VM_TOKEN;
		p++;
		break;
	     }
	     break;

	  case 'x':
	  case 'X':
	  case '0':
	  case '1':
	  case '2':
	  case '3':
	  case '4':
	  case '5':
	  case '6':
	  case '7':
	  case '8':
	  case '9':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode, mode);
	     ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                : (TokenType[TokenCount++] = NUMBER_TOKEN));
	     break;

	  case 'a':
	  case 'A':
	     tmp = p;
	     tmp++;
	     if ((*tmp == 'F' || *tmp == 'f') && (*(tmp + 1) == ' ' || *(tmp + 1) == '=' ))
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & AF_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = AF_TOKEN;
		p++;
		p++;
		break;
	     }
	     if ((*tmp == 'C' || *tmp == 'c') && (*(tmp + 1) == ' ' || *(tmp + 1) == '=' ))
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & AC_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = AC_TOKEN;
		p++;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode, mode);
	     ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                : (TokenType[TokenCount++] = NUMBER_TOKEN));
	     break;


	  case 'b':
	  case 'B':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode, mode);
	     ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                : (TokenType[TokenCount++] = NUMBER_TOKEN));
	     break;

	  case 'c':
	  case 'C':
	     tmp = p;
	     tmp++;
	     if (*tmp == 'S' || *tmp == 's')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tCS;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = CS_TOKEN;
		p++;
		p++;
		break;
	     }
	     if ((*tmp == 'F' || *tmp == 'f') && (*(tmp + 1) == ' ' || *(tmp + 1) == '=' ))
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & CF_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = CF_TOKEN;
		p++;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode, mode);
	     ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                : (TokenType[TokenCount++] = NUMBER_TOKEN));
	     break;

	  case 'd':
	  case 'D':
	     tmp = p;
	     tmp++;
	     if (*tmp == 'S' || *tmp == 's')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tDS;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = DS_TOKEN;
		p++;
		p++;
		break;
	     }
	     if ((*tmp == 'F' || *tmp == 'f') && (*(tmp + 1) == ' ' || *(tmp + 1) == '=' ))
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & DF_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = DF_TOKEN;
		p++;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode, mode);
	     ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                : (TokenType[TokenCount++] = NUMBER_TOKEN));
	     break;

	  case 'e':
	  case 'E':
	     tmp = p;
	     tmp++;
	     if (*tmp == 'A' || *tmp == 'a')
	     {
		tmp++;
		if (*tmp == 'X' || *tmp == 'x')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tEAX;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = EAX_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode,
                                                   mode);
	        ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                   : (TokenType[TokenCount++] = NUMBER_TOKEN));
		break;
	     }
	     if (*tmp == 'B' || *tmp == 'b')
	     {
		tmp++;
		if (*tmp == 'X' || *tmp == 'x')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tEBX;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = EBX_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
		if (*tmp == 'P' || *tmp == 'p')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tEBP;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = EBP_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode,
                                                   mode);
	        ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                   : (TokenType[TokenCount++] = NUMBER_TOKEN));
		break;
	     }
	     if (*tmp == 'C' || *tmp == 'c')
	     {
		tmp++;
		if (*tmp == 'X' || *tmp == 'x')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tECX;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = ECX_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode,
                                                   mode);
	        ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                   : (TokenType[TokenCount++] = NUMBER_TOKEN));
		break;
	     }
	     if (*tmp == 'D' || *tmp == 'd')
	     {
		tmp++;
		if (*tmp == 'X' || *tmp == 'x')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tEDX;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = EDX_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
		if (*tmp == 'I' || *tmp == 'i')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tEDI;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = EDI_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode,
                                                   mode);
	        ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                   : (TokenType[TokenCount++] = NUMBER_TOKEN));
		break;
	     }
	     if (*tmp == 'S' || *tmp == 's')
	     {
		tmp++;
		if (*tmp == 'P' || *tmp == 'p')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tESP;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = ESP_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
		if (*tmp == 'I' || *tmp == 'i')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tESI;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = ESI_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tES;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = ES_TOKEN;
		p++;
		p++;
		break;
	     }
	     if (*tmp == 'I' || *tmp == 'i')
	     {
		tmp++;
		if (*tmp == 'P' || *tmp == 'p')
		{
		   if (stackFrame)
		      TokenValue[TokenCount] = stackFrame->tEIP;
		   TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		   TokenType[TokenCount++] = EIP_TOKEN;
		   p++;
		   p++;
		   p++;
		   break;
		}
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode, mode);
	     ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                : (TokenType[TokenCount++] = NUMBER_TOKEN));
	     break;

	  case 'f':
	  case 'F':
	     tmp = p;
	     tmp++;
	     if (*tmp == 'S' || *tmp == 's')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tFS;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = FS_TOKEN;
		p++;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode, mode);
	     ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                : (TokenType[TokenCount++] = NUMBER_TOKEN));
	     break;

	  case 'g':
	  case 'G':   /* GS */
	     p++;
	     if (*p == 'S' || *p == 's')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tGS;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = GS_TOKEN;
		p++;
		break;
	     }
	     break;

	  case 's':
	  case 'S':
	     p++;
	     if (*p == 'S' || *p == 's')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSS;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = SS_TOKEN;
		p++;
		break;
	     }
	     if (*p == 'F' || *p == 'f')
	     {
		if (stackFrame)
		   TokenValue[TokenCount] = stackFrame->tSystemFlags & SF_FLAG;
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = SF_TOKEN;
		p++;
		break;
	     }
	     break;

	  case '[':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = DREF_OPEN_TOKEN;
	     p++;
	     if (*p == 'D' || *p == 'd')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = DWORD_TOKEN;
		p++;
		break;
	     }
	     if (*p == 'W' || *p == 'w')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = WORD_TOKEN;
		p++;
		break;
	     }
	     if (*p == 'B' || *p == 'b')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = BYTE_TOKEN;
		p++;
		break;
	     }
	     break;

	  case '=':
	     p++;
	     if (*p == '=')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = EQUAL_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = ASSIGNMENT_TOKEN;
	     break;

	  case '<':
	     p++;
	     if (*p == '<')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = LEFT_SHIFT_TOKEN;
		p++;
		break;
	     }
	     if (*p == '=')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = LESS_EQUAL_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = LESS_TOKEN;
	     break;

	  case '>':
	     p++;
	     if (*p == '>')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = RIGHT_SHIFT_TOKEN;
		p++;
		break;
	     }
	     if (*p == '=')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = GREATER_EQUAL_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = GREATER_TOKEN;
	     break;

	  case '|':
	     p++;
	     if (*p == '|')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = LOGICAL_OR_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = OR_TOKEN;
	     break;

	  case '&':
	     p++;
	     if (*p == '&')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = LOGICAL_AND_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = AND_TOKEN;
	     break;

	  default: /* if we get a default, drop the character on the floor */
	     p++;
	     break;

	 }
       }
       else
       {
	 switch (*p)
	 {
	  case '\0':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = NULL_TOKEN;
	     return (p);

	  case ']':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = DREF_CLOSE_TOKEN;
	     p++;
	     break;

	  case '(':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = BB_TOKEN;
	     p++;
	     break;

	  case ')':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = EB_TOKEN;
	     p++;
	     break;

	  case '+':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = PLUS_TOKEN;
	     p++;
	     break;

	  case '-':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = MINUS_TOKEN;
	     p++;
	     break;

	  case '*':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = MULTIPLY_TOKEN;
	     p++;
	     break;

	  case '/':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = DIVIDE_TOKEN;
	     p++;
	     break;

	  case '%':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = MOD_TOKEN;
	     p++;
	     break;

	  case '~':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = NEG_TOKEN;
	     p++;
	     break;

	  case '^':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = XOR_TOKEN;
	     p++;
	     break;

	  case '!':
	     p++;
	     if (*p == '=')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = NOT_EQUAL_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = NOT_TOKEN;
	     break;

	  case ' ':   /* drop spaces on the floor */
	     p++;
	     break;

	  /*
	   *  These cases require special handling
	   */

	  case 'x':
	  case 'X':
	  case '0':
	  case '1':
	  case '2':
	  case '3':
	  case '4':
	  case '5':
	  case '6':
	  case '7':
	  case '8':
	  case '9':
	  case 'a':
	  case 'A':
	  case 'b':
	  case 'B':
	  case 'c':
	  case 'C':
	  case 'd':
	  case 'D':
	  case 'e':
	  case 'E':
	  case 'f':
	  case 'F':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenValue[TokenCount] = GetNumber(p, &p, &delta, &retCode, mode);
	     ((retCode) ? (TokenType[TokenCount++] = INVALID_NUMBER_TOKEN)
	                : (TokenType[TokenCount++] = NUMBER_TOKEN));
	     break;

	  case '[':
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = DREF_OPEN_TOKEN;
	     p++;
	     if (*p == 'D' || *p == 'd')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = DWORD_TOKEN;
		p++;
		break;
	     }
	     if (*p == 'W' || *p == 'w')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = WORD_TOKEN;
		p++;
		break;
	     }
	     if (*p == 'B' || *p == 'b')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = BYTE_TOKEN;
		p++;
		break;
	     }
	     break;

	  case '=':
	     p++;
	     if (*p == '=')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = EQUAL_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = ASSIGNMENT_TOKEN;
	     break;

	  case '<':
	     p++;
	     if (*p == '<')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = LEFT_SHIFT_TOKEN;
		p++;
		break;
	     }
	     if (*p == '=')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = LESS_EQUAL_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = LESS_TOKEN;
	     break;

	  case '>':
	     p++;
	     if (*p == '>')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = RIGHT_SHIFT_TOKEN;
		p++;
		break;
	     }
	     if (*p == '=')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = GREATER_EQUAL_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = GREATER_TOKEN;
	     break;

	  case '|':
	     p++;
	     if (*p == '|')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = LOGICAL_OR_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = OR_TOKEN;
	     break;

	  case '&':
	     p++;
	     if (*p == '&')
	     {
		TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
		TokenType[TokenCount++] = LOGICAL_AND_TOKEN;
		p++;
		break;
	     }
	     TokenIndex[TokenCount] = (unsigned long) ((unsigned long) p - (unsigned long) op);
	     TokenType[TokenCount++] = AND_TOKEN;
	     break;

	  default: /* if we get a default, drop the character on the floor */
	     p++;
	     break;

	 }
       }
    }
    return p;

}

void displayExpressionHelp(void)
{

       if (DBGPrint("Arithmetic Operators\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("+   add\n")) return;
       if (DBGPrint("-   subtract\n")) return;
       if (DBGPrint("*   multiply\n")) return;
       if (DBGPrint("/   divide\n")) return;
       if (DBGPrint("<<  bit shift left\n")) return;
       if (DBGPrint(">>  bit shift right\n")) return;
       if (DBGPrint("|   OR operator\n")) return;
       if (DBGPrint("&   AND operator\n")) return;
       if (DBGPrint("^   XOR operator\n")) return;
       if (DBGPrint("~   NEG operator\n")) return;
       if (DBGPrint("%%   MODULO operator\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 1:\n")) return;
       if (DBGPrint("(0)> .e (100 + 100)\n")) return;
       if (DBGPrint("(0)> result = 0x200 (512)\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 2:\n")) return;
       if (DBGPrint("(0)> .e (1 << 20)\n")) return;
       if (DBGPrint("(0)> result = 0x00100000 (1,024,000)\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 3:\n")) return;
       if (DBGPrint("(0)> .e (FEF023 & 100F)\n")) return;
       if (DBGPrint("(0)> result = 0x1003 (4099)\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Boolean Operators (Conditional Breakpoint)\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("==      is equal to\n")) return;
       if (DBGPrint("!=      is not equal to\n")) return;
       if (DBGPrint("!<expr> is not\n")) return;
       if (DBGPrint(">       is greater than\n")) return;
       if (DBGPrint("<       is less than\n")) return;
       if (DBGPrint(">=      is greater than or equal to\n")) return;
       if (DBGPrint("<=      if less than or equal to\n")) return;
       if (DBGPrint("||      logical OR operator\n")) return;
       if (DBGPrint("&&      logical AND operator\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("all breakpoint conditions must be enclosed in brackets () to\n")) return;
       if (DBGPrint("evaluate correctly\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 1 (Execute Breakpoint):\n")) return;
       if (DBGPrint("(0)> b 37000 (EAX == 20 && EBX <= 4000)\n")) return;
       if (DBGPrint("breakpoint will activate if condition is true (returns 1)\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 2 (IO Breakpoint):\n")) return;
       if (DBGPrint("(0)> bi 3D4 (!EBX && [d ESI+40] != 2000)\n")) return;
       if (DBGPrint("breakpoint will activate if condition is true (returns 1)\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Register Operators\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("EAX, EBX, ECX, EDX        - general registers\n")) return;
       if (DBGPrint("ESI, EDI, EBP, ESP        - pointer registers\n")) return;
       if (DBGPrint("EIP, <symbol>             - instruction pointer or symbol\n")) return;
       if (DBGPrint("CS, DS, ES, FS, GS, SS    - segment registers\n")) return;
       if (DBGPrint("CF, PF, AF, ZF, SF, IF    - flags\n")) return;
       if (DBGPrint("DF, OF, VM, AC\n")) return;
       if (DBGPrint("=                         - set equal to\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 1:\n")) return;
       if (DBGPrint("(0)> EAX = 0032700 \n")) return;
       if (DBGPrint("EAX changed to 0x0032700\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 2:\n")) return;
       if (DBGPrint("(0)> u thread_switch\n")) return;
       if (DBGPrint("unassembles function thread_switch\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 3 (Dump):\n")) return;
       if (DBGPrint("(0)> d EBP+ECX\n")) return;
       if (DBGPrint("(dumps [d EBP + ECX])\n")) return;
       if (DBGPrint("[addr] 00 00 00 01 02 04 07 ...\n")) return;
       if (DBGPrint("[addr] 00 34 56 00 7A 01 00 ...\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Bracket Operators\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("(       begin expression bracket\n")) return;
       if (DBGPrint(")       end expression bracket\n")) return;
       if (DBGPrint("[       begin pointer\n")) return;
       if (DBGPrint("]       end pointer\n")) return;
       if (DBGPrint("d       DWORD reference\n")) return;
       if (DBGPrint("w       WORD reference\n")) return;
       if (DBGPrint("b       BYTE reference\n")) return;
       if (DBGPrint("<num>r  parse number as decimal not hex flag"
                    " (e.g. 512r == 200)\n")) return;
       if (DBGPrint("Note - DWORD,WORD, and BYTE dereference operators must\n"))          return;
       if (DBGPrint("immediately follow pointer brackets (no spaces)\n"))                 return;
       if (DBGPrint("i.e.  [d <addr/symbol>] or [w <addr/symbol>] or\n"))
           return;
       if (DBGPrint("[b <addr/symbol>], etc.\n"))
           return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 1 (dump):\n")) return;
       if (DBGPrint("(0)> d [d EAX+100r] \n")) return;
       if (DBGPrint("[eax + 100 (dec)] 00 00 00 01 02 04 07 00\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 2 (dump):\n")) return;
       if (DBGPrint("(0)> d [w 003400] \n")) return;
       if (DBGPrint("[addr (hex)] 00 22 00 01 02 04 07 ...\n")) return;
       if (DBGPrint("[addr (hex)] 00 31 A1 00 6A 05 00 ...\n")) return;
       if (DBGPrint("\n")) return;
       if (DBGPrint("Example 3 (break):\n")) return;
       if (DBGPrint("(0)> b = 7A000 (EAX + ECX == 30) && ([d B8000+50]  == 0x07)\n")) return;
       if (DBGPrint("breakpoint will activate if condition is true (returns 1)\n")) return;
       if (DBGPrint("\n")) return;

       return;

}

unsigned long deref(unsigned long type, unsigned long value)
{
   unsigned long *pd;
   unsigned short *pw;
   unsigned char *pb;

   switch (type)
   {
      case ULONG_PTR:
	 pd = (unsigned long *) value;
#if (DEBUG_EXPRESS)
	 DBGPrint("dref (d): [%08X]-> %08X\n", pd,
                  mdb_getword((unsigned long)pd, 4));
#endif
	 return (unsigned long) mdb_getword((unsigned long)pd, 4);

      case WORD_PTR:
	 pw = (unsigned short *) value;
#if (DEBUG_EXPRESS)
	 DBGPrint("dref (w): [%08X]-> %04X\n", pw,
                  mdb_getword((unsigned long)pw, 2));
#endif
	 return (unsigned short) mdb_getword((unsigned long)pw, 2);

      case BYTE_PTR:
	 pb = (unsigned char *) value;
#if (DEBUG_EXPRESS)
	 DBGPrint("dref (b): [%08X]-> %02X\n", pb,
                  mdb_getword((unsigned long)pb, 1));
#endif
	 return (unsigned char) mdb_getword((unsigned long)pb, 1);

      default:
#if (DEBUG_EXPRESS)
	 DBGPrint("dref (?): [0]-> 0\n");
#endif
	 return 0;
   }

}

unsigned long ExpressPush(unsigned long i)
{
     if (sp > bos)
     {
#if (DEBUG_EXPRESS)
	DBGPrint("push : <err>\n");
#endif
	return 0;
     }
     *sp = i;
#if (DEBUG_EXPRESS)
     DBGPrint("push : %08X (%d)\n", *sp, *sp);
#endif
     sp++;
     return 1;
}

unsigned long ExpressPop(void)
{
    sp--;
    if (sp < tos)
    {
       sp++;
#if (DEBUG_EXPRESS)
       DBGPrint("pop  : <err>\n");
#endif
       return 0;
    }
#if (DEBUG_EXPRESS)
    DBGPrint("pop  : %08X (%d)\n", *sp, *sp);
#endif
    return *sp;

}

unsigned long ContextPush(unsigned long i)
{
     if (c_sp > c_bos)
     {
#if (DEBUG_EXPRESS)
	DBGPrint("cpush: <err>\n");
#endif
	return 0;
     }
     *c_sp = i;
#if (DEBUG_EXPRESS)
     DBGPrint("cpush: %08X (%d)\n", *c_sp, *c_sp);
#endif
     c_sp++;
     return 1;
}

unsigned long ContextPop(void)
{
    c_sp--;
    if (c_sp < c_tos)
    {
       c_sp++;
#if (DEBUG_EXPRESS)
       DBGPrint("cpop : <err>\n");
#endif
       return 0;
    }
#if (DEBUG_EXPRESS)
    DBGPrint("cpop : %08X (%d)\n", *c_sp, *c_sp);
#endif
    return *c_sp;

}

unsigned long BooleanPush(unsigned long i)
{
     if (b_sp > b_bos)
     {
#if (DEBUG_BOOL_STACK)
	DBGPrint("bpush: <err>\n");
#endif
	return 0;
     }
     *b_sp = i;
#if (DEBUG_BOOL_STACK)
     DBGPrint("bpush: %08X (%d)\n", *b_sp, *b_sp);
#endif
     b_sp++;
     return 1;
}

unsigned long BooleanPop(void)
{
    b_sp--;
    if (b_sp < b_tos)
    {
       b_sp++;
#if (DEBUG_BOOL_STACK)
       DBGPrint("bpop : <err>\n");
#endif
       return 0;
    }
#if (DEBUG_BOOL_STACK)
    DBGPrint("bpop : %08X (%d)\n", *b_sp, *b_sp);
#endif
    return *b_sp;

}

unsigned long LogicalPush(unsigned long i)
{
     if (l_sp > l_bos)
     {
#if (DEBUG_LOGICAL_STACK)
	DBGPrint("lpush: <err>\n");
#endif
	return 0;
     }
     *l_sp = i;
#if (DEBUG_LOGICAL_STACK)
     DBGPrint("lpush: %08X (%d)\n", *l_sp, *l_sp);
#endif
     l_sp++;
     return 1;
}

unsigned long LogicalPop(void)
{
    l_sp--;
    if (l_sp < l_tos)
    {
       l_sp++;
#if (DEBUG_LOGICAL_STACK)
       DBGPrint("lpop : <err>\n");
#endif
       return 0;
    }
#if (DEBUG_LOGICAL_STACK)
    DBGPrint("lpop : %08X (%d)\n", *l_sp, *l_sp);
#endif
    return *l_sp;

}

void initNumericStacks(void)
{

    sp = numStack;
    tos = sp;
    bos = sp + NUM_STACK_SIZE - 1;

    c_sp = contextStack;
    c_tos = c_sp;
    c_bos = c_sp + CONTEXT_STACK_SIZE - 1;

    b_sp = booleanStack;
    b_tos = b_sp;
    b_bos = b_sp + BOOL_STACK_SIZE - 1;

    l_sp = logicalStack;
    l_tos = l_sp;
    l_bos = l_sp + LOGICAL_STACK_SIZE - 1;

}

unsigned long ProcessOperator(unsigned long oper)
{
    unsigned long a, b;

    b = ExpressPop();
    a = ExpressPop();
    switch(oper)
    {
       case NEG_TOKEN:
	  break;

       case LEFT_SHIFT_TOKEN:
	  ExpressPush(a << (b % PROCESSOR_32));  /* mod (b) to base 32 */
	  break;

       case RIGHT_SHIFT_TOKEN:
	  ExpressPush(a >> (b % PROCESSOR_32));  /* mob (b) to base 32 */
	  break;

       case PLUS_TOKEN:
	  ExpressPush(a + b);
	  break;

       case XOR_TOKEN:
	  ExpressPush(a ^ b);
	  break;

       case AND_TOKEN:
	  ExpressPush(a & b);
	  break;

       case MOD_TOKEN:
	  if (b) /* if modulo by zero, drop value on the floor */
	     ExpressPush(a % b);
	  else
	     ExpressPush(0);
	  break;

       case OR_TOKEN:
	  ExpressPush(a | b);
	  break;

       case MINUS_TOKEN:
	  ExpressPush(a - b);
	  break;

       case MULTIPLY_TOKEN:
	  ExpressPush(a * b);
	  break;

       case DIVIDE_TOKEN:
	  if (b) /* if divide by zero, drop value on the floor */
	     ExpressPush(a / b);
	  else
	     ExpressPush(0);
	  break;

    }
    return 0;

}

unsigned long ProcessBoolean(unsigned long oper)
{

    unsigned long a, b;

    b = ExpressPop();
    a = ExpressPop();
    switch(oper)
    {
       case NOT_TOKEN:
	  ExpressPush(a == b); /* we pushed an imaginary zero on the stack */
	  break;             /* this operation returns the boolean for (!x) */

       case GREATER_TOKEN:
	  ExpressPush(a > b);
	  break;

       case LESS_TOKEN:
	  ExpressPush(a < b);
	  break;

       case GREATER_EQUAL_TOKEN:
	  ExpressPush(a >= b);
	  break;

       case LESS_EQUAL_TOKEN:
	  ExpressPush(a <= b);
	  break;

       case EQUAL_TOKEN:
	  ExpressPush(a == b);
	  break;

       case NOT_EQUAL_TOKEN:
	  ExpressPush(a != b);
	  break;
    }
    return 0;

}

unsigned long ProcessLogical(unsigned long oper)
{

    unsigned long a, b;

    b = ExpressPop();
    a = ExpressPop();
    switch(oper)
    {
       case LOGICAL_AND_TOKEN:
	  ExpressPush(a && b);
	  break;

       case LOGICAL_OR_TOKEN:
	  ExpressPush(a || b);
	  break;
    }
    return 0;

}

unsigned long ParseLogical(unsigned long logicalCount)
{

    register int i, r;
    unsigned long a;
    unsigned long c = 0, lastClass = 0, oper = 0;

    for (i = 0; i < logicalCount; i++)
       ExpressPush(LogicalPop());

    for (i = 0, r = 0; i < (logicalCount / 2); i++)
    {
       a = ExpressPop();
       TokenType[r] = NUMBER_TOKEN;
       TokenValue[r++] = a;
       a = ExpressPop();
       TokenType[r] = a;  /* get the operator type */
       TokenValue[r++] = 0;
    }

    initNumericStacks();

#if (DEBUG_LOGICAL)
     DBGPrint("\n");
#endif
    for (i = 0; i < logicalCount; i++)
    {
#if DEBUG_LOGICAL
       DBGPrint("token: %02X  value: %08X  type: %s\n", TokenType[i],
	      TokenValue[i], parserDescription[TokenType[i]]);
#endif
       switch (TokenType[i])
       {
	  case LOGICAL_AND_TOKEN:
	  case LOGICAL_OR_TOKEN:
	     if (lastClass != CLASS_BOOLEAN)
	     {
		lastClass = CLASS_BOOLEAN;
		oper = TokenType[i];
	     }
	     continue;

	  case NUMBER_TOKEN:
	     if (lastClass == CLASS_DATA)
	     {
		c = ExpressPop();
		return c;
	     }
	     lastClass = CLASS_DATA;
	     c = TokenValue[i];
	     ExpressPush(c);
	     if (oper)
		oper = ProcessLogical(oper);
	     continue;

	  case NULL_TOKEN:
	     c = ExpressPop();
	     return c;

	  default:
	     continue;
       }
    }
    return c;

}

unsigned long ParseBoolean(unsigned long booleanCount)
{

    register int i, r;
    unsigned long a, oper = 0;
    unsigned long c = 0, lastClass = 0, logicalCount = 0;

    for (i = 0; i < booleanCount; i++)
       ExpressPush(BooleanPop());

    for (i = 0, r = 0; i < (booleanCount / 2); i++)
    {
       a = ExpressPop();
       TokenType[r] = NUMBER_TOKEN;
       TokenValue[r++] = a;
       a = ExpressPop();
       TokenType[r] = a;  /* get the operator type */
       TokenValue[r++] = 0;
    }

    initNumericStacks();

#if (DEBUG_BOOL)
     DBGPrint("\n");
#endif
    for (i = 0; i < booleanCount; i++)
    {
#if DEBUG_BOOL
       DBGPrint("token: %02X  value: %08X  type: %s\n", TokenType[i],
	      TokenValue[i], parserDescription[TokenType[i]]);
#endif
       switch (TokenType[i])
       {
	  /* partition operators */
	  case LOGICAL_AND_TOKEN:
	  case LOGICAL_OR_TOKEN:
	     c = ExpressPop();
	     LogicalPush(c);
	     logicalCount++;
	     LogicalPush(TokenType[i]);
	     logicalCount++;
	     ExpressPush(c);
	     oper = 0;
	     lastClass = 0;
	     continue;

	  /* boolean operators */
	  case NOT_TOKEN:
	     if (lastClass != CLASS_BOOLEAN)
	     {
		ExpressPush(0);
		lastClass = CLASS_BOOLEAN;
		oper = TokenType[i];
	     }
	     continue;

	  case GREATER_TOKEN:
	  case LESS_TOKEN:
	  case GREATER_EQUAL_TOKEN:
	  case LESS_EQUAL_TOKEN:
	  case EQUAL_TOKEN:
	  case NOT_EQUAL_TOKEN:
	     if (lastClass != CLASS_BOOLEAN)
	     {
		lastClass = CLASS_BOOLEAN;
		oper = TokenType[i];
	     }
	     continue;

	  case NUMBER_TOKEN:
	     if (lastClass == CLASS_DATA)
	     {
		c = ExpressPop();
		if (logicalCount)
		{
		   LogicalPush(c);
		   logicalCount++;
		   LogicalPush(0); /* push null token */
		   logicalCount++;
		   c = ParseLogical(logicalCount);
		   return c;
		}
		return c;
	     }
	     lastClass = CLASS_DATA;
	     c = TokenValue[i];
	     ExpressPush(c);
	     if (oper)
		oper = ProcessBoolean(oper);
	     continue;

	  case NULL_TOKEN:
	     c = ExpressPop();
	     if (logicalCount)
	     {
		LogicalPush(c);
		logicalCount++;
		LogicalPush(0); /* push null token */
		logicalCount++;
		c = ParseLogical(logicalCount);
		return c;
	     }
	     return c;

	  default:
	     continue;
       }
    }
    return c;

}

unsigned long Evaluate(StackFrame *stackFrame, unsigned char **p, unsigned long *type, unsigned long mode)
{

     register int i;
     unsigned long oper = 0, dref = 0, bracket = 0;
     unsigned long dref_type = 0, lastClass = 0, lastToken = 0;
     unsigned long neg_flag = 0, negative_flag = 0, c;
     unsigned long booleanCount = 0;

#ifdef MDB_ATOMIC
     spin_lock_irqsave(&expressLock, flags);
#endif

     if (type)
	*type = INVALID_EXPRESSION;
#if (DEBUG_BOOL)
     DBGPrint("\n");
#endif
#if (DEBUG_EXPRESS)
     DBGPrint("\np: %08X  %s\n", *p, *p);
#endif
     parseTokens(stackFrame, *p, mode);
     if (TokenCount)
     {
	initNumericStacks();
	for (i = 0; i < TokenCount; i++)
	{
#if (DEBUG_EXPRESS)
	   DBGPrint("token: %s  lastClass: %d\n", parserDescription[TokenType[i]], lastClass);
#endif
	   switch (TokenType[i])
	   {
	      case INVALID_NUMBER_TOKEN:
                 goto evaluate_error_exit;

	      case NOT_TOKEN:
		 if (lastClass != CLASS_DATA)
		 {
		    if (oper)
		       oper = ProcessOperator(oper);
		    c = ExpressPop();
		    BooleanPush(c);
		    booleanCount++;
		    BooleanPush(TokenType[i]);
		    booleanCount++;
		    dref_type = 0;
		    lastClass = 0;
		    neg_flag  = 0;
		    negative_flag = 0;
		 }
		 lastToken = NOT_TOKEN;
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
		    oper = ProcessOperator(oper);
		 c = ExpressPop();
		 BooleanPush(c);
		 booleanCount++;
		 BooleanPush(TokenType[i]);
		 booleanCount++;
		 dref_type = 0;
		 lastClass = 0;
		 neg_flag  = 0;
		 negative_flag = 0;
		 lastToken = 0;
		 continue;

	      /* partition operators */
	      case DWORD_TOKEN:
		 if (dref)
		    dref_type = ULONG_PTR;
		 lastToken = 0;
		 continue;

	      case WORD_TOKEN:
		 if (dref)
		    dref_type = WORD_PTR;
		 lastToken = 0;
		 continue;

	      case BYTE_TOKEN:
		 if (dref)
		    dref_type = BYTE_PTR;
		 lastToken = 0;
		 continue;

	      case DREF_OPEN_TOKEN:   /* push state and nest for de-reference */
		 if (lastClass == CLASS_DATA)
		 {
		    *p = (unsigned char *)((unsigned long)*p + (unsigned long)TokenIndex[i]);
		    if (type)
		    {
		       if (booleanCount)
			  *type = BOOLEAN_EXPRESSION;
		       else
			  *type = NUMERIC_EXPRESSION;
		    }
		    c = ExpressPop();
		    if (booleanCount)
		    {
		       BooleanPush(c);
		       booleanCount++;
		       BooleanPush(0); /* last operator is the null token */
		       booleanCount++;
		       c = ParseBoolean(booleanCount);
#if (DEBUG_BOOL)
		       DBGPrint("be_N : (%d) = (%s)\n", c, c ? "TRUE" : "FALSE");
#endif
#ifdef MDB_ATOMIC
		       spin_unlock_irqrestore(&expressLock, flags);
#endif
		       return c;
		    }
#if (DEBUG_EXPRESS)
		    DBGPrint("ee_N : %08X (%d)\n", c, c);
#endif
#ifdef MDB_ATOMIC
		    spin_unlock_irqrestore(&expressLock, flags);
#endif
		    return c;
		 }
		 dref++;
		 ContextPush(dref_type);
		 ContextPush(oper);
		 ContextPush(lastClass);
		 ContextPush(neg_flag);
		 ContextPush(negative_flag);
		 dref_type = 0;
		 oper      = 0;
		 lastClass = 0;
		 neg_flag  = 0;
		 negative_flag = 0;
		 lastToken = 0;
		 continue;

	      case DREF_CLOSE_TOKEN: /* pop state,restore,and complete oper */
		 if (!dref)
		    continue;
		 c = deref(dref_type, ExpressPop());
		 ExpressPush(c);
		 negative_flag  = ContextPop();
		 neg_flag  = ContextPop();
		 ContextPop();
		 oper      = ContextPop();
		 dref_type = ContextPop();
		 if (dref)
		    dref--;
		 lastClass = CLASS_DATA;

		 c = ExpressPop();
		 if (negative_flag)
		    c = 0 - c;
		 if (neg_flag)
		    c = ~c;
		 neg_flag = 0;
		 negative_flag = 0;
		 ExpressPush(c);

		 if (oper)
		    oper = ProcessOperator(oper);
		 lastToken = 0;
		 continue;

	      case BB_TOKEN:
		 if (lastClass == CLASS_DATA)
		 {
		    *p = (unsigned char *)((unsigned long)*p + (unsigned long)TokenIndex[i]);
		    if (type)
		    {
		       if (booleanCount)
			  *type = BOOLEAN_EXPRESSION;
		       else
			  *type = NUMERIC_EXPRESSION;
		    }
		    c = ExpressPop();
		    if (booleanCount)
		    {
		       BooleanPush(c);
		       booleanCount++;
		       BooleanPush(0); /* last operator is the null token */
		       booleanCount++;
		       c = ParseBoolean(booleanCount);
#if (DEBUG_BOOL)
		       DBGPrint("be_N : (%d) = (%s)\n", c, c ? "TRUE" : "FALSE");
#endif
#ifdef MDB_ATOMIC
		       spin_unlock_irqrestore(&expressLock, flags);
#endif
		       return c;
		    }
#if (DEBUG_EXPRESS)
		    DBGPrint("ee_N : %08X (%d)\n", c, c);
#endif
#ifdef MDB_ATOMIC
		    spin_unlock_irqrestore(&expressLock, flags);
#endif
		    return c;
		 }
		 bracket++;
		 ContextPush(oper);
		 ContextPush(lastClass);
		 ContextPush(neg_flag);
		 ContextPush(negative_flag);
		 oper      = 0;
		 lastClass = 0;
		 neg_flag  = 0;
		 negative_flag = 0;
		 lastToken = 0;
		 continue;

	      case EB_TOKEN:
		 if (!bracket)
		    continue;
		 negative_flag  = ContextPop();
		 neg_flag  = ContextPop();
		 ContextPop();
		 oper      = ContextPop();
		 if (bracket)
		    bracket--;
		 lastClass = CLASS_DATA;
		 c = ExpressPop();
		 if (negative_flag)
		    c = 0 - c;
		 if (neg_flag)
		    c = ~c;
		 neg_flag = 0;
		 negative_flag = 0;
		 ExpressPush(c);
		 if (oper)
		    oper = ProcessOperator(oper);
		 lastToken = 0;
		 continue;

	      /* arithmetic operators */
	      case NEG_TOKEN:
		 neg_flag = 1;
		 lastToken = 0;
		 continue;

	      case MINUS_TOKEN:
		 if (lastClass == CLASS_ARITHMETIC)
		 {
		    lastToken = MINUS_TOKEN;
		    negative_flag = 1;
		    continue;
		 }
		 if (lastClass != CLASS_ARITHMETIC)
		 {
		    lastClass = CLASS_ARITHMETIC;
		    oper = TokenType[i];
		 }
		 lastToken = 0;
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
		 if (lastClass != CLASS_ARITHMETIC)
		 {
		    lastClass = CLASS_ARITHMETIC;
		    oper = TokenType[i];
		 }
		 lastToken = 0;
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
	      case EIP_TOKEN:
	      case FLAGS_TOKEN:
	      case EAX_TOKEN:
	      case EBX_TOKEN:
	      case ECX_TOKEN:
	      case EDX_TOKEN:
	      case ESI_TOKEN:
	      case EDI_TOKEN:
	      case EBP_TOKEN:
	      case ESP_TOKEN:
	      case CS_TOKEN:
	      case DS_TOKEN:
	      case ES_TOKEN:
	      case FS_TOKEN:
	      case GS_TOKEN:
	      case SS_TOKEN:
	      case NUMBER_TOKEN:
		 if (lastClass == CLASS_DATA)
		 {
		    *p = (unsigned char *)((unsigned long)*p + (unsigned long)TokenIndex[i]);
		    if (type)
		    {
		       if (booleanCount)
			  *type = BOOLEAN_EXPRESSION;
		       else
			  *type = NUMERIC_EXPRESSION;
		    }
		    c = ExpressPop();
		    if (booleanCount)
		    {
		       BooleanPush(c);
		       booleanCount++;
		       BooleanPush(0); /* last operator is the null token */
		       booleanCount++;
		       c = ParseBoolean(booleanCount);
#if (DEBUG_BOOL)
		       DBGPrint("be_N : (%d) = (%s)\n", c, c ? "TRUE" : "FALSE");
#endif
#ifdef MDB_ATOMIC
		       spin_unlock_irqrestore(&expressLock, flags);
#endif
		       return c;
		    }
#if (DEBUG_EXPRESS)
		    DBGPrint("ee_N : %08X (%d)\n", c, c);
#endif
#ifdef MDB_ATOMIC
		    spin_unlock_irqrestore(&expressLock, flags);
#endif
		    return c;
		 }
		 lastClass = CLASS_DATA;
		 c = TokenValue[i];
		 if (negative_flag)
		    c = 0 - c;
		 if (neg_flag)
		    c = ~TokenValue[i];
		 neg_flag = 0;
		 negative_flag = 0;
		 ExpressPush(c);
		 if (oper)
		    oper = ProcessOperator(oper);
		 lastToken = 0;
		 continue;

	      case NULL_TOKEN:
		 *p = (unsigned char *)((unsigned long)*p + (unsigned long)TokenIndex[i]);
		 if (TokenCount > 1 && type)
		 {
		    if (booleanCount)
		       *type = BOOLEAN_EXPRESSION;
		    else
		       *type = NUMERIC_EXPRESSION;
		 }
		 c = ExpressPop();
		 if (booleanCount)
		 {
		    BooleanPush(c);
		    booleanCount++;
		    BooleanPush(0); /* last operator is the null token */
		    booleanCount++;
		    c = ParseBoolean(booleanCount);
#if (DEBUG_BOOL)
		    DBGPrint("be_N : (%d) = (%s)\n", c, c ? "TRUE" : "FALSE");
#endif
#ifdef MDB_ATOMIC
		    spin_unlock_irqrestore(&expressLock, flags);
#endif
		    return c;
		 }
#if (DEBUG_EXPRESS)
		 DBGPrint("ee_N : %08X (%d)\n", c, c);
#endif
#ifdef MDB_ATOMIC
		 spin_unlock_irqrestore(&expressLock, flags);
#endif
		 return c;

	      /* assignment operators */
	      case ASSIGNMENT_TOKEN:
		 lastToken = 0;
		 continue;

	      default:
		 lastToken = 0;
		 continue;
	   }
	}
     }

evaluate_error_exit:
     if (type)
	*type = INVALID_EXPRESSION;

     if (lastToken) {};

#ifdef MDB_ATOMIC
     spin_unlock_irqrestore(&expressLock, flags);
#endif
     return 0;

}

unsigned long EvaluateNumericExpression(StackFrame *stackFrame, unsigned char **p, unsigned long *type)
{
     register unsigned long c;
     c = Evaluate(stackFrame, p, type, 1);
     return c;
}

unsigned long EvaluateExpression(StackFrame *stackFrame, unsigned char **p, unsigned long *type)
{
     register unsigned long c;
     c = Evaluate(stackFrame, p, type, 0);
     return c;
}

void EvaluateCommandExpression(StackFrame *stackFrame, unsigned char *p)
{
     unsigned char *expr;
     unsigned long type;
     unsigned long c;

#if DEBUG_EXPRESS
     DBGPrint("expr: [%s]\n", p);
#endif
     expr = p;
     c = EvaluateExpression(stackFrame, &p, &type);
     if (type)
     {
	DBGPrint("expr: %s = 0x%08X (%dr) (%s) bool(%i) = %s\n",
		    expr, (unsigned)c, (int)c, exprDescription[type & 3],
		    (c) ? 1 : 0, (c) ? "TRUE" : "FALSE");
     }
     else
     {
        DBGPrint("expression parameters invalid\n");
	DBGPrint("expr: %s = 0x%08X (%dr) (results invalid) (%s)"
                 " bool(%i) = %s\n",
		 expr, (unsigned)c, (int)c, exprDescription[type & 3],
		 (c) ? 1 : 0, (c) ? "TRUE" : "FALSE");
     }
     return;

}
