
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

#define mod(a)  (((a) >> 6) & 7)
#define reg(a)  (((a) >> 3) & 7)
#define rm(a)   ((a) & 7)
#define ss(a)   (((a) >> 6) & 7)
#define indx(a) (((a) >> 3) & 7)
#define base(a) ((a) & 7)

#define   CF_FLAG   0x00000001
#define   PF_FLAG   0x00000004
#define   AF_FLAG   0x00000010
#define   ZF_FLAG   0x00000040
#define   SF_FLAG   0x00000080
#define   TF_FLAG   0x00000100
#define   IF_FLAG   0x00000200
#define   DF_FLAG   0x00000400
#define   OF_FLAG   0x00000800
#define   NT_FLAG   0x00004000
#define   RF_FLAG   0x00010000
#define   VM_FLAG   0x00020000
#define   AC_FLAG   0x00040000
#define   VIF_FLAG  0x00080000
#define   VIP_FLAG  0x00100000
#define   ID_FLAG   0x00200000

short SegmentSize = 32;
unsigned char buf[20];
unsigned char *VirtualAddress;
short bufp, bufe;
unsigned char ubuf[4000], *ubufp;
short Columns;
unsigned char NoNameBuffer[100];
short DefaultPickSign;
short Prefix;
signed int modrmv;
signed int sibv;
short OperandSize;
short AddressSize;
unsigned long needs_proceed;
unsigned long jmp_active;
short MODRMExtend;
long DeRefValue;
long pSize;
extern unsigned long full_deref_toggle;
extern unsigned long debug_deref;

void ProcessInstruction(StackFrame *stackFrame, unsigned char *s);

/* Percent tokens in strings:
   First char after '%':
	A - direct address
	C - reg of r/m picks control register
	D - reg of r/m picks debug register
        E - r/m picks operand
        F - flags register
	G - reg of r/m picks general register
        I - immediate data (takes extended size, data size)
        J - relative IP offset
        M - r/m picks memory
        O - no r/m, offset only
	R - mod of r/m picks register only
        S - reg of r/m picks segment register
        T - reg of r/m picks test register
        X - DS:ESI
        Y - ES:EDI
	2 - Prefix of two-byte opcode
	e - put in 'e' if use32 (second char is part of reg name)
	    put in 'w' for use16 or 'd' for use32 (second char is 'w')
	f - floating point (second char is esc value)
	g - do r/m group 'n'
	p - Prefix
	s - size override (second char is a,o)
        + - make default signed
   Second char after '%':
        a - two words in memory (BOUND)
	b - byte
        c - byte or word
        d - dword
	p - 32 or 48 bit pointer
        s - six byte pseudo-descriptor
        v - word or dword
	w - word
        F - use floating regs in mod/rm
        + - always sign
        - - sign if negative
	1-8 - group number, esc value, etc
*/

unsigned char *opmap1[] =
{
  /* 0 */
  "ADD %Eb,%Gb",   "ADD %Ev,%Gv",    "ADD %Gb,%Eb",   "ADD %Gv,%Ev",
  "ADD AL,%I-bb",  "ADD %eAX,%I-vv", "PUSH ES",       "POP ES",
  "OR %Eb,%Gb",    "OR %Ev,%Gv",     "OR %Gb,%Eb",    "OR %Gv,%Ev",
  "OR AL,%Ibb",    "OR %eAX,%Ivv",   "PUSH CS",       "%2 ",
  /* 1 */
  "ADC %Eb,%Gb",   "ADC %Ev,%Gv",    "ADC %Gb,%Eb",   "ADC %Gv,%Ev",
  "ADC AL,%I-bb",  "ADC %eAX,%I-vv", "PUSH SS",       "POP SS",
  "SBB %Eb,%Gb",   "SBB %Ev,%Gv",    "SBB %Gb,%Eb",   "SBB %Gv,%Ev",
  "SBB AL,%I-bb",  "SBB %eAX,%I-vv", "PUSH DS",       "POP DS",
  /* 2 */
  "AND %Eb,%Gb",   "AND %Ev,%Gv",    "AND %Gb,%Eb",   "AND %Gv,%Ev",
  "AND AL,%Ibb",   "AND %eAX,%Ivv",  "%pe",           "DAA",
  "SUB %Eb,%Gb",   "SUB %Ev,%Gv",    "SUB %Gb,%Eb",   "SUB %Gv,%Ev",
  "SUB AL,%I-bb",  "SUB %eAX,%I-vv", "%pc",           "DAS",
  /* 3 */
  "XOR %Eb,%Gb",   "XOR %Ev,%Gv",    "XOR %Gb,%Eb",   "XOR %Gv,%Ev",
  "XOR AL,%Ibb",   "XOR %eAX,%Ivv",  "%ps",           "AAA",
  "CMP %Eb,%Gb",   "CMP %Ev,%Gv",    "CMP %Gb,%Eb",   "CMP %Gv,%Ev",
  "CMP AL,%I-bb",  "CMP %eAX,%I-vv", "%pd",           "AAS",
  /* 4 */
  "INC %eAX",      "INC %eCX",       "INC %eDX",      "INC %eBX",
  "INC %eSP",      "INC %eBP",       "INC %eSI",      "INC %eDI",
  "DEC %eAX",      "DEC %eCX",       "DEC %eDX",      "DEC %eBX",
  "DEC %eSP",      "DEC %eBP",       "DEC %eSI",      "DEC %eDI",
  /* 5 */
  "PUSH %eAX",     "PUSH %eCX",      "PUSH %eDX",     "PUSH %eBX",
  "PUSH %eSP",     "PUSH %eBP",      "PUSH %eSI",     "PUSH %eDI",
  "POP %eAX",      "POP %eCX",       "POP %eDX",      "POP %eBX",
  "POP %eSP",      "POP %eBP",       "POP %eSI",      "POP %eDI",
  /* 6 */
  "PUSHA",         "POPA",           "BOUND %Gv,%Ma", "ARPL %Ew,%Rw",
  "%pf",           "%pg",            "%so",           "%sa",
  "PUSH %I-vv",    "IMUL %Gv=%Ev*%I-vv", "PUSH %I-vb","IMUL %Gv=%Ev*%I-vb",
  "INSB %Yb,DX",   "INS%ew %Yv,DX",  "OUTSB DX,%Xb",  "OUTS%ew DX,%Xv",
  /* 7 */
  "JO %Jb",        "JNO %Jb",        "JC %Jb",        "JNC %Jb",
  "JZ %Jb",        "JNZ %Jb",        "JBE %Jb",       "JNBE %Jb",
  "JS %Jb",        "JNS %Jb",        "JPE %Jb",       "JPO %Jb",
  "JL %Jb",        "JGE %Jb",        "JLE %Jb",       "JG %Jb",
  /* 8 */
  "%g1 %Eb,%Ibb",  "%g1 %Ev,%Ivv",   0,               "%g1 %Ev,%Ivb",
  "TEST %Eb,%Gb",  "TEST %Ev,%Gv",   "XCHG %Eb,%Gb",  "XCHG %Ev,%Gv",
  "MOV %Eb,%Gb",   "MOV %Ev,%Gv",    "MOV %Gb,%Eb",   "MOV %Gv,%Ev",
  "MOV %Ew,%Sw",   "LEA %Gv,%M ",    "MOV %Sw,%Ew",   "POP %Ev",
  /* 9 */
  "NOP",           "XCHG %eAX,%eCX", "XCHG %eAX,%eDX","XCHG %eAX,%eBX",
  "XCHG %eAX,%eSP","XCHG %eAX,%eBP", "XCHG %eAX,%eSI","XCHG %eAX,%eDI",
  "CBW",           "CWD",            "CALL %Ap",      "FWAIT",
  "PUSH %eFLAGS",  "POP %eFLAGS",    "SAHF",          "LAHF",
  /* a */
  "MOV AL,%Ob",    "MOV %eAX,%Ov",   "MOV %Ob,AL",    "MOV %Ov,%eAX",
  "MOVSB %Xb,%Yb", "MOVS%ew %Xv,%Yv","CMPSB %Xb,%Yb", "CMPS%ew %Xv,%Yv",
  "TEST AL,%Ibb",  "TEST %eAX,%Ivv", "STOSB %Yb,AL",  "STOS%ew %Yv,%eAX",
  "LODSB AL,%Xb",  "LODS%ew %eAX,%Xv","SCASB AL,%Xb", "SCAS%ew %eAX,%Xv",
  /* b */
  "MOV AL,%Ibb",   "MOV CL,%Ibb",    "MOV DL,%Ibb",   "MOV BL,%Ibb",
  "MOV AH,%Ibb",   "MOV CH,%Ibb",    "MOV DH,%Ibb",   "MOV BH,%Ibb",
  "MOV %eAX,%I-vv","MOV %eCX,%I-vv", "MOV %eDX,%I-vv","MOV %eBX,%I-vv",
  "MOV %eSP,%Ivv", "MOV %eBP,%Ivv",  "MOV %eSI,%I-vv","MOV %eDI,%I-vv",
  /* c */
  "%g2 %Eb,%Ibb",  "%g2 %Ev,%Ibb",   "RET %Iw",       "RET",
  "LES %Gv,%Mp",   "LDS %Gv,%Mp",    "MOV %Eb,%Ibb",  "MOV %Ev,%I-vv",
  "ENTER %Iww,%Ibb","LEAVE",         "RETF %Iww",     "RETF",
  "INT 3",         "INT %Ibb",       "INTO",          "IRET",
  /* d */
  "%g2 %Eb,1",     "%g2 %Ev,1",      "%g2 %Eb,CL",    "%g2 %Ev,CL",
  "AAM %Ibb",      "AAD %Ibb",       0,               "XLAT",
  "%f0",           "%f1",            "%f2",           "%f3",
  "%f4",           "%f5",            "%f6",           "%f7",
  /* e */
  "LOOPNE %Jb",    "LOOPE %Jb",      "LOOP %Jb",      "JCXZ %Jb",
  "IN AL,%Ibb",    "IN %eAX,%Ibb",   "OUT %Ibb,AL",   "OUT %Ibb,%eAX",
  "CALL %Jv",      "JMP %Jv",        "JMP %Ap",       "JMP(S) %Jb",
  "IN AL,DX",      "IN %eAX,DX",     "OUT DX,AL",     "OUT DX,%eAX",
  /* f */
  "LOCK %p ",      0,                "REPNE %p ",     "REP(E) %p ",
  "HLT",           "CMC",            "%g3",           "%g0",
  "CLC",           "STC",            "CLI",           "STI",
  "CLD",           "STD",            "%g4",           "%g5"
};

unsigned char *second[] =
{
  /* 0 */
  "%g6",           "%g7",            "LAR %Gv,%Ew",   "LSL %Gv,%Ew",
  0,               0,                "CLTS",          0,
  "INVD",          "WBINV",          0,               "UD2A",
  0,               "%y0",            "FEMMS",         0,
  /* 1 */
  "%z8",	   "%z9",	     "MOVLPS",	      "MOVLPS",
  "UNPCKLPS",	   "UNPCKHPS",	     "MOVHPS",	      "MOVHPS",
  "%g14",	   0,		     0,		      0,
  0,		   0,		     0,		      0,
  /* 2 */
  "MOV %Rd,%Cd",   "MOV %Rd,%Dd",    "MOV %Cd,%Rd",   "MOV %Dd,%Rd",
  "MOV %Rd,%Td",   0,                "MOV %Td,%Rd",   0,
  "MOVAPS",	   "MOVAPS",	     "%z2",	      "MOVNTPS",
  "%z4",	   "%z3",	     "UCOMISS",       "COMISS",
  /* 3 */
  "WRMSR MSR[ECX],EDX:EAX", "RDTSC", "RDMSR EDX:EAX,MSR[ECX]",  "RDPMC",
  "SYSENTER",      "SYSEXIT",	     0,		      0,
  0,		   0,		     0,		      0,
  0,               0,                0,               0,
  /* 4 */
  "CMOVO %Gv,%Ev", "CMOVNO %Gv,%Ev", "CMOVB %Gv,%Ev",  "CMOVAE %Gv,%Ev",
  "CMOVE %Gv,%Ev", "CMOVNE %Gv,%Ev", "CMOVBE %Gv,%Ev", "CMOVA %Gv,%Ev",
  "CMOVS %Gv,%Ev", "CMOVNS %Gv,%Ev", "CMOVP %Gv,%Ev",  "CMOVNP %Gv,%Ev",
  "CMOVL %Gv,%Ev", "CMOVGE %Gv,%Ev", "CMOVLE %Gv,%Ev", "CMOVG %Gv,%Ev",
  /* 5 */
  "MOVMSKPS",	   "%z13",           "%z12",          "%z11",
  "ANDPS",         "ANDNPS",         "ORPS",          "XORPS",
  "%z0",	   "%z10",           0,               0,
  "%z14",          "%z7",            "%z5",           "%z6",
  /* 6 */
  0,               0,                0,               0,
  0,               0,                0,               0,
  0,               0,                0,               0,
  0,               0,               "MOVD %Pd,%Ed",  "MOVQ %Pq,%Qq",
  /* 7 */
  0,		   0,                0,               0,
  0,               0,                0,               0,
  0,		   0,                0,               0,
  0,               0,               "MOVD %Ed,%Pd",  "MOVQ %Qq,%Pq",
  /* 8 */
  "JO %Jv",        "JNO %Jv",        "JC %Jv",        "JNC %Jv",
  "JZ %Jv",        "JNZ %Jv",        "JBE %Jv",       "JNBE %Jv",
  "JS %Jv",        "JNS %Jv",        "JPE %Jv",       "JPO %Jv",
  "JL %Jv",        "JGE %Jv",        "JLE %Jv",       "JG %Jv",
  /* 9 */
  "SETO %Eb",      "SETNO %Eb",      "SETC %Eb",      "SETNC %Eb",
  "SETZ %Eb",      "SETNZ %Eb",      "SETBE %Eb",     "SETNBE %Eb",
  "SETS %Eb",      "SETNS %Eb",      "SETP %Eb",      "SETNP %Eb",
  "SETL %Eb",      "SETGE %Eb",      "SETLE %Eb",     "SETG %Eb",
  /* a */
  "PUSH FS",       "POP FS",         "CPUID",         "BT %Ev,%Gv",
  "SHLD %Ev,%Gv,%Ibb","SHLD %Ev,%Gv,CL", 0,           0,
  "PUSH GS",       "POP GS",         "RSM",           "BTS %Ev,%Gv",
  "SHRD %Ev,%Gv,%Ibb","SHRD %Ev,%Gv,CL", 0,           "IMUL %Gv,%Ev",

  /* b */
  "CMPXCHG %Eb,%Gb", "CMPXCHG %Ev,%Gv", "LSS %Mp",       "BTR %Ev,%Gv",
  "LFS %Mp",         "LGS %Mp",         "MOVZX %Gv,%Eb", "MOVZX %Gv,%Ew",
  0,                 "UD2B",            "%g8 %Ev,%Ibb",  "BTC %Ev,%Gv",
  "BSF %Gv,%Ev",     "BSR %Gv,%Ev",     "MOVSX %Gv,%Eb", "MOVSX %Gv,%Ew",

  /* c */
  "XADD %Eb,%Gb",  "XADD %Ev,%Gv",   0,		      0,
  0,		   0,                0,               "%g9",
  "BSWAP %eAX",    "BSWAP %eCX",     "BSWAP %eDX",    "BSWAP %eBX",
  "BSWAP %eSP",    "BSWAP %eBP",     "BSWAP %eSI",    "BSWAP %eDI",
  /* d */
  0,		   0,                0,               0,
  0,		   0,		     0,		      0,
  0,		   0,		     0,		      0,
  0,		   0,		     0,		      0,
  /* e */
  0,		   0,		     0,		      0,
  0,		   0,		     0,		      0,
  0,		   0,		     0,		      0,
  0,		   0,		     0,		      0,
  /* f */
  0,		   0,		     0,		      0,
  0,		   0,		     0,		      0,
  0,		   0,		     0,		      0,
  0,		   0,		     0,		      0

};

unsigned char *groups[][10] = {     /* group 0 is group 3 for %Ev set */
/* g0 */
{ "TEST %Ev,%Ivv", "TEST %Ev,%Ivv,", "NOT %Ev",       "NEG %Ev",
  "MUL %eAX,%Ev",  "IMUL %eAX,%Ev",  "DIV %eAX,%Ev",  "IDIV %eAX,%Ev" },

/* g1 */
{ "ADD%+-",        "OR",             "ADC%+-",        "SBB%+-",
  "AND",           "SUB%+-",         "XOR",           "CMP%+-" },

/* g2  */
{ "ROL",           "ROR",            "RCL",           "RCR",
  "SHL",           "SHR",            "SHL",           "SAR" },

/* g3 */
{ "TEST %Eb,%Ibb", "TEST %Eb,%Ibb,", "NOT %Eb",       "NEG %Eb",
  "MUL AL,%Eb",    "IMUL AL,%Eb",    "DIV AL,%Eb",    "IDIV AL,%Eb" },

/* g4 */
{ "INC %Eb",       "DEC %Eb",        0,               0,
  0,               0,                0,               0 },

/* g5 */
{ "INC %Ev",       "DEC %Ev",        "CALL %Ev",      "CALL %Ep",
  "JMP %Ev",       "JMP %Ep",        "PUSH %Ev",      0 },

/* g6 */
{ "SLDT %Ew",      "STR %Ew",        "LLDT %Ew",      "LTR %Ew",
  "VERR %Ew",      "VERW %Ew",       0, 0 },

/* g7 */
{ "SGDT %Ms",      "SIDT %Ms",       "LGDT %Ms",      "LIDT %Ms",
  "SMSW %Ew",      0,                "LMSW %Ew",      "INVLPG" },

/* g8 */
{ 0,               0,                0, 0,
  "BT",            "BTS",            "BTR",           "BTC" },

/* g9 */
{ 0,               "CMPXCH8B %Mq",   0,               0,
  0,               0,                0,               0 },

/* g10 */
{ 0,               0,                0,               0,
  0,               0,                0,               0 },

};

/* zero here means invalid.  If first entry starts with '*', use st(i) */
/* no assumed %EFs here.  Indexed by rm(modrm()) */

unsigned char *f0[] = {
  0,               0,                0,               0,
  0,		   0,		     0,	              0
};

unsigned char *fop_9[]  = {
  "*FXCH ST,%GF" };

unsigned char *fop_10[] = {
  "FNOP",	   0,		      0,	      0,
  0,		   0,		      0,	      0 };

unsigned char *fop_12[] = {
  "FCHS",          "FABS",            0,              0,
  "FTST",          "FXAM",	      0,	      0 };

unsigned char *fop_13[] = {
  "FLD1",	   "FLDL2T",	      "FLDL2E",       "FLDPL",
  "FLDLG2",	   "FLDLN2",	      "FLDZ",	      0 };

unsigned char *fop_14[] = {
  "F2XM1",	   "FYL2X",	      "FPTAN",	      "FPATAN",
  "FXTRACT",	   "FPREM1",	      "FDECSTP",      "FINCSTP" };

unsigned char *fop_15[] = {
  "FPREM",	   "FYL2XP1",	      "FSQRT",	      "FSINCOS",
  "FRNDINT",	   "FSCALE",	      "FSIN",	      "FCOS" };

unsigned char *fop_21[] = {
   0, "FUCOMPP", 0, 0,
   0, 0, 0, 0 };

unsigned char *fop_28[] = {
   0, 0, "FCLEX", "FINIT",
   0, 0, 0, 0 };

unsigned char *fop_32[] = {
   "*FADD %GF,ST" };

unsigned char *fop_33[] = {
   "*FMUL %GF,ST" };

unsigned char *fop_36[] = {
   "*FSUBR %GF,ST" };

unsigned char *fop_37[] = {
   "*FSUB %GF,ST" };

unsigned char *fop_38[] = {
   "*FDIVR %GF,ST" };

unsigned char *fop_39[] = {
   "*FDIV %GF,ST" };

unsigned char *fop_40[] = {
   "*FFREE %GF" };

unsigned char *fop_42[] = { "*FST %GF" };

unsigned char *fop_43[] = { "*FSTP %GF" };

unsigned char *fop_44[] = { "*FUCOM %GF" };

unsigned char *fop_45[] = { "*FUCOMP %GF" };

unsigned char *fop_48[] = { "*FADDP %GF,ST" };

unsigned char *fop_49[] = { "*FMULP %GF,ST" };

unsigned char *fop_51[] = { 0, "FCOMPP", 0, 0, 0, 0, 0, 0 };

unsigned char *fop_52[] = { "*FSUBRP %GF,ST" };

unsigned char *fop_53[] = { "*FSUBP %GF,ST" };

unsigned char *fop_54[] = { "*FDIVRP %GF,ST" };

unsigned char *fop_55[] = { "*FDIVP %GF,ST" };

unsigned char *fop_60[] = { "FSTSW AX", 0, 0, 0, 0, 0, 0, 0 };

unsigned char **fspecial[] = { /* 0=use st(i), 1=undefined 0 in fop_* means undefined */
  0, 0, 0, 0, 0, 0, 0, 0,
  0, fop_9, fop_10, 0, fop_12, fop_13, fop_14, fop_15,
  f0, f0, f0, f0, f0, fop_21, f0, f0,
  f0, f0, f0, f0, fop_28, f0, f0, f0,
  fop_32, fop_33, f0, f0, fop_36, fop_37, fop_38, fop_39,
  fop_40, f0, fop_42, fop_43, fop_44, fop_45, f0, f0,
  fop_48, fop_49, f0, fop_51, fop_52, fop_53, fop_54, fop_55,
  f0, f0, f0, f0, fop_60, f0, f0, f0,
  };

unsigned char *floatops[] = { /* assumed " %EF" at end of each.  mod != 3 only */
/*00*/ "FADD", "FMUL", "FCOM", "FCOMP",
       "FSUB", "FSUBR", "FDIV", "FDIVR",
/*08*/ "FLD", 0, "FST", "FSTP",
       "FLDENV", "FLDCW", "FSTENV", "FSTCW",
/*16*/ "FIADD", "FIMUL", "FICOMW", "FICOMPW",
       "FISUB", "FISUBR", "FIDIV", "FIDIVR",
/*24*/ "FILD", 0, "FIST", "FISTP",
       "FRSTOR", "FLDT", 0, "FSTPT",
/*32*/ "FADDQ", "FMULQ", "FCOMQ", "FCOMPQ",
       "FSUBQ", "FSUBRQ", "FDIVQ", "FDIVRQ",
/*40*/ "FLDQ", 0, "FSTQ", "FSTPQ",
       "FRESTOR", 0, "FSAVE", "FSTSW",
/*48*/ "FIADDW", "FIMULW", "FICOMW", "FICOMPW",
       "FISUBW", "FISUBRW", "FIDIVW", "FIDIVR",
/*56*/ "FILDW", 0, "FISTW", "FISTPW",
       "FBLDT", "FILDQ", "FBSTPT", "FISTPQ"
};


unsigned char *reg_names[3][8]={
  {"AL","CL","DL","BL","AH","CH","DH","BH"},
  {"AX","CX","DX","BX","SP","BP","SI","DI"},
  {"EAX", /* 0 */
   "ECX", /* 1 */
   "EDX", /* 2 */
   "EBX", /* 3 */
   "ESP", /* 4 */
   "EBP", /* 5 */
   "ESI", /* 6 */
   "EDI"} /* 7 */
   };

unsigned char *r_str[] = {
   "BX+SI",
   "BX+DI",
   "BP+SI",
   "BP+DI",
   "SI",
   "DI",
   "BP",
   "BX"
};

unsigned char *formats[5][4] = {
    {"%08X", "%08X", "%08z", "%08z" },
    {"%02X", "%02X", "%02z", "%02z" },
    {"%04X", "%04X", "%04z", "%04z" },
    {"%08X", "%08X", "%08z", "%08z" },
    {"%08X", "%08X", "%08z", "%08z" } };

unsigned char *i_str[] = {
   "+EAX", /* 0 */
   "+ECX", /* 1 */
   "+EDX", /* 2 */
   "+EBX", /* 3 */
   "",     /* 4 */
   "+EBP", /* 5 */
   "+ESI", /* 6 */
   "+EDI"  /* 7 */
};

unsigned char *sib_str[] = {
   "%p:[EAX", /* 0 */
   "%p:[ECX", /* 1 */
   "%p:[EDX", /* 2 */
   "%p:[EBX", /* 3 */
   "%p:[ESP", /* 4 */
   0,         /* 5 */
   "%p:[ESI", /* 6 */
   "%p:[EDI"  /* 7 */
};

long reg_get_value(unsigned long index, StackFrame *stackFrame)
{
     if (!stackFrame)
        return 0;

     switch (index)
     {
	case 0:
	   return stackFrame->tEAX;

	case 1:
	   return stackFrame->tECX;

	case 2:
	   return stackFrame->tEDX;

	case 3:
	   return stackFrame->tEBX;

	case 4:
	   return stackFrame->tESP;

	case 5:
	   return stackFrame->tEBP;

	case 6:
	   return stackFrame->tESI;

	case 7:
	   return stackFrame->tEDI;

	default:
	   return 0;
     }
}

long sib_get_value(unsigned long index, StackFrame *stackFrame)
{
     if (!stackFrame)
        return 0;

     switch (index)
     {
	case 0:
	   return stackFrame->tEAX;

	case 1:
	   return stackFrame->tECX;

	case 2:
	   return stackFrame->tEDX;

	case 3:
	   return stackFrame->tEBX;

	case 4:
	   return stackFrame->tESP;

	case 6:
	   return stackFrame->tESI;

	case 7:
	   return stackFrame->tEDI;

	case 5:
	default:
	   return 0;
     }
}

long istr_get_value(unsigned long index, StackFrame *stackFrame)
{
     if (!stackFrame)
        return 0;

     switch (index)
     {
	case 0:
	   return stackFrame->tEAX;

	case 1:
	   return stackFrame->tECX;

	case 2:
	   return stackFrame->tEDX;

	case 3:
	   return stackFrame->tEBX;

	case 5:
	   return stackFrame->tEBP;

	case 6:
	   return stackFrame->tESI;

	case 7:
	   return stackFrame->tEDI;

	case 4:
	default:
	   return 0;
     }
}


unsigned char *output_address(void *val, unsigned long *delta)
{
    unsigned char *symbolName;
    unsigned char *moduleName;
    register int c = get_processor_id();

    if (delta)
      *delta = 0;

    symbolName = GetSymbolFromValue((unsigned long) val, &symbuf[c][0],
                                    MAX_SYMBOL_LEN);
    if (symbolName)
    {
       moduleName = GetModuleInfoFromSymbolValue((unsigned long) val, &modbuf[c][0],
                                                 MAX_SYMBOL_LEN);
       if (moduleName)
          sprintf(NoNameBuffer, "%s|%s=%08X", moduleName, symbolName,
                 (unsigned)val);
       else
          sprintf(NoNameBuffer, "%s=%08X", symbolName, (unsigned)val);
    }
    else
       sprintf(NoNameBuffer, "%08X", (unsigned)val);

    return NoNameBuffer;

}

unsigned char *output_jmp_address(StackFrame *stackFrame, long val, long *delta)
{

    unsigned char *symbolName;
    unsigned char *moduleName;
    long va = (long) VirtualAddress;
    long v;
    long segment, offset, sym_offset = 0;
    register int c = get_processor_id();

    if (delta)
      *delta = 0;

    if (!stackFrame)
       return NoNameBuffer;

    if ((!strncmp(ubuf, "JBE", 3) &&
           ((stackFrame->tSystemFlags & ZF_FLAG) ||
            (stackFrame->tSystemFlags & CF_FLAG))) ||

	(!strncmp(ubuf, "JCXZ", 4) &&
           (!stackFrame->tECX)) ||

	(!strncmp(ubuf, "JC", 2) &&
           (stackFrame->tSystemFlags & CF_FLAG)) ||

	(!strncmp(ubuf, "JLE", 3) &&
           ((stackFrame->tSystemFlags & ZF_FLAG) ||
           ((stackFrame->tSystemFlags & ~SF_FLAG) !=
            (stackFrame->tSystemFlags & ~OF_FLAG)))) ||

	(!strncmp(ubuf, "JL", 2) &&
           ((stackFrame->tSystemFlags & ~SF_FLAG) !=
            (stackFrame->tSystemFlags & ~OF_FLAG))) ||

	(!strncmp(ubuf, "JMP", 3)) ||

	(!strncmp(ubuf, "JGE", 3) &&
           ((stackFrame->tSystemFlags & ~SF_FLAG) ==
            (stackFrame->tSystemFlags & ~OF_FLAG))) ||

	(!strncmp(ubuf, "JG", 2) &&
           ((!(stackFrame->tSystemFlags & ZF_FLAG)) &&
            ((stackFrame->tSystemFlags & ~SF_FLAG) ==
            (stackFrame->tSystemFlags & ~OF_FLAG)))) ||

	(!strncmp(ubuf, "JNBE", 4) &&
            (!(stackFrame->tSystemFlags & CF_FLAG)) &&
            (!(stackFrame->tSystemFlags & ZF_FLAG))) ||

	(!strncmp(ubuf, "JNO", 3) &&
           (!(stackFrame->tSystemFlags & OF_FLAG))) ||

	(!strncmp(ubuf, "JNC", 3) &&
           (!(stackFrame->tSystemFlags & CF_FLAG))) ||

	(!strncmp(ubuf, "JNZ", 3) &&
           (!(stackFrame->tSystemFlags & ZF_FLAG))) ||

	(!strncmp(ubuf, "JNS", 3) &&
           (!(stackFrame->tSystemFlags & SF_FLAG))) ||

	(!strncmp(ubuf, "JO", 2) &&
           (stackFrame->tSystemFlags & OF_FLAG)) ||

	(!strncmp(ubuf, "JPE", 3) &&
           (stackFrame->tSystemFlags & PF_FLAG)) ||

	(!strncmp(ubuf, "JPO", 3) &&
           (!(stackFrame->tSystemFlags & PF_FLAG))) ||

	(!strncmp(ubuf, "JS", 2) &&
           (stackFrame->tSystemFlags & SF_FLAG)) ||

	(!strncmp(ubuf, "JZ", 2) &&
           (stackFrame->tSystemFlags & ZF_FLAG)) ||

	(!strncmp(ubuf, "LOOPNE", 6) &&
           (stackFrame->tECX) &&
           (!(stackFrame->tSystemFlags & ZF_FLAG))) ||

	(!strncmp(ubuf, "LOOPE", 5) &&
           (stackFrame->tECX) &&
           (stackFrame->tSystemFlags & ZF_FLAG)) ||

	(!strncmp(ubuf, "LOOP", 4)))
    {
       jmp_active = 1;
       if (SegmentSize == 32)
       {
	  symbolName = GetSymbolFromValueWithOffset(val + va, &sym_offset,
                                            &symbuf[c][0], MAX_SYMBOL_LEN);
	  if (symbolName)
          {
	     moduleName = GetModuleInfoFromSymbolValue(val + va, &modbuf[c][0],
                                                       MAX_SYMBOL_LEN);
             if (moduleName)
             {
                if (sym_offset)
	           sprintf(NoNameBuffer, "%s|%s+0x%X=%08X %c",
                     moduleName, symbolName,
                     (unsigned)sym_offset, (unsigned)(val + va),
		     ((val + va) < va) ? 0x18 : 0x19);
                else
	           sprintf(NoNameBuffer, "%s|%s=%08X %c",
                     moduleName, symbolName,
                     (unsigned)(val + va),
		     ((val + va) < va) ? 0x18 : 0x19);
             }
             else
             {
                if (sym_offset)
	           sprintf(NoNameBuffer, "%s+0x%X=%08X %c", symbolName,
                     (unsigned)sym_offset, (unsigned)(val + va),
		     ((val + va) < va) ? 0x18 : 0x19);
                else
	           sprintf(NoNameBuffer, "%s=%08X %c", symbolName,
                     (unsigned)(val + va),
		     ((val + va) < va) ? 0x18 : 0x19);
             }
          }
	  else
	     sprintf(NoNameBuffer, "%08X %c",
                    (unsigned)(val + va),
		    ((val + va) < va) ? 0x18 : 0x19);
       }
       else
       {
	  symbolName = GetSymbolFromValueWithOffset(val + va, &sym_offset,
                                           &symbuf[c][0], MAX_SYMBOL_LEN);
	  if (symbolName)
          {
	     moduleName = GetModuleInfoFromSymbolValue(val + va, &modbuf[c][0],
                                                       MAX_SYMBOL_LEN);
             if (moduleName)
             {
                if (sym_offset)
	           sprintf(NoNameBuffer, "%s|%s+0x%X=%08X %c",
                     moduleName, symbolName,
                     (unsigned)sym_offset, (unsigned)(val + va),
		     ((val + va) < va) ? 0x18 : 0x19);
                else
	           sprintf(NoNameBuffer, "%s|%s=%08X %c",
                     moduleName, symbolName,
                     (unsigned)(val + va),
		     ((val + va) < va) ? 0x18 : 0x19);
             }
             else
             {
                if (sym_offset)
	           sprintf(NoNameBuffer, "%s+0x%X=%08X %c", symbolName,
                     (unsigned)sym_offset, (unsigned)(val + va),
		     ((val + va) < va) ? 0x18 : 0x19);
                else
	           sprintf(NoNameBuffer, "%s=%08X %c", symbolName,
                     (unsigned)(val + va),
		     ((val + va) < va) ? 0x18 : 0x19);
             }
          }
	  else
	  {
	     v = val + va;
	     segment = (((unsigned long)v >> 4) & 0x0000FFFF);
	     offset = ((unsigned long)v & 0x0000000F);
	     sprintf(NoNameBuffer, "%04X:%04X %c",
                    (unsigned)segment, (unsigned)offset,
		    ((val + va) < va) ? 0x18 : 0x19);
	  }
       }
    }
    else
    {
       if (SegmentSize == 32)
       {
	  symbolName = GetSymbolFromValueWithOffset(val + va, &sym_offset,
                                           &symbuf[c][0], MAX_SYMBOL_LEN);
	  if (symbolName)
          {
	     moduleName = GetModuleInfoFromSymbolValue(val + va, &modbuf[c][0],
                                                       MAX_SYMBOL_LEN);
             if (moduleName)
             {
                if (sym_offset)
	           sprintf(NoNameBuffer, "%s|%s+0x%X=%08X",
                     moduleName, symbolName,
                     (unsigned)sym_offset, (unsigned)(val + va));
                else
	           sprintf(NoNameBuffer, "%s|%s=%08X",
                     moduleName, symbolName,
                     (unsigned)(val + va));
             }
             else
             {
                if (sym_offset)
	           sprintf(NoNameBuffer, "%s+0x%X=%08X", symbolName,
                     (unsigned)sym_offset, (unsigned)(val + va));
                else
	           sprintf(NoNameBuffer, "%s=%08X", symbolName,
                      (unsigned)(val + va));
             }
          }
	  else
	     sprintf(NoNameBuffer, "%08X",
                     (unsigned)(val + va));
       }
       else
       {
	  symbolName = GetSymbolFromValueWithOffset(val + va, &sym_offset,
                                           &symbuf[c][0], MAX_SYMBOL_LEN);
	  if (symbolName)
          {
	     moduleName = GetModuleInfoFromSymbolValue(val + va, &modbuf[c][0],
                                                       MAX_SYMBOL_LEN);
             if (moduleName)
             {
                if (sym_offset)
	           sprintf(NoNameBuffer, "%s|%s+0x%X=%08X",
                     moduleName, symbolName,
                     (unsigned)sym_offset, (unsigned)(val + va));
                else
	           sprintf(NoNameBuffer, "%s|%s=%08X",
                     moduleName, symbolName,
                     (unsigned)(val + va));
             }
             else
             {
                if (sym_offset)
	           sprintf(NoNameBuffer, "%s+0x%X=%08X", symbolName,
                     (unsigned)sym_offset, (unsigned)(val + va));
                else
	           sprintf(NoNameBuffer, "%s=%08X", symbolName,
                     (unsigned)(val + va));
             }
          }
	  else
	  {
	     v = val + va;
	     segment = (((unsigned long)v >> 4) & 0x0000FFFF);
	     offset = ((unsigned long)v & 0x0000000F);
	     sprintf(NoNameBuffer, "%04X:%04X",
                     (unsigned)segment, (unsigned)offset);
	  }
       }
    }
    return NoNameBuffer;

}

unsigned short read_memory(void *addr, void *buf, unsigned len)
{
    register unsigned long i;
    register unsigned char *s = buf;

    for (i = 0; i < len; i++)
       if (s)
          s[i] = (unsigned char)mdb_getword((unsigned long)((unsigned long)addr + i), 1);
    return 0;
}

unsigned char getbyte(void)
{
    short s;

    if (bufp >= bufe)
    {
       s = 4;  /* byte read window is 4 */
       read_memory((void *)VirtualAddress, buf, s);
       bufe = s;
       bufp = 0;
    }
    VirtualAddress = (void *)((unsigned long) VirtualAddress + (unsigned long) 1);

    DBGPrint("%02X", buf[bufp]);
    Columns += 2;

    return buf[bufp++];
}

unsigned short modrm(void)
{
    if (modrmv == -1)
      modrmv = getbyte();

    return modrmv;
}

unsigned short sib(void)
{
    if (sibv == -1)
      sibv = getbyte();

    return sibv;
}

void DebugPrint(char *s, ...)
{
    char **a = &s;

    vsprintf(ubufp, s, (va_list)(a+1));

    while (*ubufp) ubufp++;
}

void DebugPutChar(unsigned char c)
{
    if (c == '\t')
    {
      do {
	*ubufp++ = ' ';
      } while ((ubufp-ubuf) % 8);
    }
    else
      *ubufp++ = c;
    *ubufp = 0;
}

unsigned long dereference_address(unsigned long addr, unsigned long width)
{
    register unsigned long retCode;

    retCode = mdb_verify_rw((void *)addr, width);
    if (full_deref_toggle)
    {
       switch (retCode)
       {
	  case 0:
	     if (width == 1)
		DebugPrint("=(*%08X=%02X)", addr, (unsigned char)mdb_getword(addr, 1));
	     else
	     if (width == 2)
		DebugPrint("=(*%08X=%04X)", addr, (unsigned short)mdb_getword(addr, 2));
	     else
	     if (width == 4)
		DebugPrint("=(*%08X=%08X)", addr, (unsigned long)mdb_getword(addr, 4));
	     else
		DebugPrint("=(*%08X=?)", addr);
	     return retCode;

	  default:
	     if (width == 1)
		DebugPrint("=(*%08X=?)", addr);
	     else
	     if (width == 2)
		DebugPrint("=(*%08X=?)", addr);
	     else
	     if (width == 4)
		DebugPrint("=(*%08X=?)", addr);
	     else
		DebugPrint("=(*%08X=?)", addr);
	     return retCode;
       }
    }
    else
    {
       switch (retCode)
       {
	  case 0:
	     if (width == 1)
		DebugPrint("=%02X", (unsigned char)mdb_getword(addr, 1));
	     else
	     if (width == 2)
		DebugPrint("=%04X", (unsigned short)mdb_getword(addr, 2));
	     else
	     if (width == 4)
		DebugPrint("=%08X", (unsigned long)mdb_getword(addr, 4));
	     else
		DebugPrint("=?");
	     return retCode;

	  default:
	     if (width == 1)
		DebugPrint("=?");
	     else
	     if (width == 2)
		DebugPrint("=?");
	     else
	     if (width == 4)
		DebugPrint("=?");
	     else
		DebugPrint("=?");
	     return retCode;
       }
    }

}

short bytes(char c)
{
    switch (c)
    {
      case 'b':
	return 1;

      case 'w':
	return 2;

      case 'd':
	return 4;

      case 'v':
	if (OperandSize == 32)
	  return 4;
	else
	  return 2;
    }
    return 0;

}

void OutputHex(char c, short extend, short optional, short defsize,
	       short sign, long *deref)
{

    unsigned char *fmt, *p;
    short n=0, s=0, i;
    long delta;
    unsigned char buf1[6];
    unsigned char *name;
    unsigned char fmt2[16];

    fmt = formats[0][sign];

    if (deref) {};

    switch (c)
    {
      case 'a':
	break;

      case 'b':
	n = 1;
	break;

      case 'w':
	n = 2;
	break;

      case 'd':
	n = 4;
	break;

      case 's':
	n = 6;
	break;

      case 'c':
      case 'v':
	if (defsize == 32)
	  n = 4;
	else
	  n = 2;
	break;

      case 'p':
	if (defsize == 32)
	  n = 6;
	else
	  n = 4;
	s = 1;
	break;

      case 'x':
	return;
    }

    for (i = 0; i < n; i++)
       buf1[i] = getbyte();

    for (; i < extend; i++)
       buf1[i] = (buf[i-1] & 0x80) ? 0xff : 0;

    if (s)
    {
       DebugPrint("%02X%02X:", buf1[n-1], buf1[n-2]);
       n -= 2;
    }
    switch (n)
    {
       case 1:
	  delta = *(char *)buf1;
	  break;

       case 2:
	  delta = *(short *)buf1;
	  break;

       case 4:
	  delta = *(long *)buf1;
	  break;
    }
    if (extend > n)
    {
       if (delta || !optional)
       {
	  if (extend <= 4)
	     fmt = formats[extend][sign];

	  if (deref)
	     *deref += delta;

          p = strchr(fmt, 'z');
          if (p)
          {
             strcpy(fmt2, fmt);
             p = strchr(fmt2, 'z');
             if (delta < 0)
                DebugPrint("-", delta);
             else
                DebugPrint("+", delta);
             *p = 'X';
             DebugPrint(fmt2, delta);
          }
          else
             DebugPrint(fmt, delta);
       }
       return;
    }
    if ((n == 4) && sign < 2)
    {
       if (deref)
	  *deref += delta;

       name = output_address((void  *)delta, (unsigned long *) &delta);
       if (name)
       {
	  DebugPrint("%s", name);
	  if (delta)
	     DebugPrint("+%X", delta);
	  return;
       }
    }
    switch (n)
    {

       case 1:
	  fmt = formats[n][sign];

          p = strchr(fmt, 'z');
          if (p)
          {
             strcpy(fmt2, fmt);
             p = strchr(fmt2, 'z');
             if (delta < 0)
                DebugPrint("-", delta);
             else
                DebugPrint("+", delta);
             *p = 'X';
	     DebugPrint(fmt2, (char) delta);
          }
          else
	     DebugPrint(fmt, (char) delta);

	  if (deref)
	     *deref += (delta & 0xFF);

	  break;

       case 2:
	  fmt = formats[n][sign];

          p = strchr(fmt, 'z');
          if (p)
          {
             strcpy(fmt2, fmt);
             p = strchr(fmt2, 'z');
             if (delta < 0)
                DebugPrint("-", delta);
             else
                DebugPrint("+", delta);
             *p = 'X';
	     DebugPrint(fmt2, (unsigned short) delta);
          }
          else
	     DebugPrint(fmt, (unsigned short) delta);

	  if (deref)
	     *deref += (delta & 0xFFFF);

	  break;

       case 4:
	  fmt = formats[n][sign];

          p = strchr(fmt, 'z');
          if (p)
          {
             strcpy(fmt2, fmt);
             p = strchr(fmt2, 'z');
             if (delta < 0)
                DebugPrint("-");
             else
                DebugPrint("+");
             *p = 'X';
	     DebugPrint(fmt2, (unsigned long) delta);
          }
          else
	     DebugPrint(fmt, (unsigned long) delta);

	  if (deref)
	     *deref += delta;

	  break;
    }

}


void reg_name(short which, char size)
{

    if (size == 'F')
    {
       DebugPrint("ST(%d)", which);
       return;
    }

    if (((size == 'v') && (OperandSize == 32)) || (size == 'd'))
    {
       DebugPutChar('E');
    }

    if (size == 'b')
    {
       DebugPutChar("ACDBACDB"[which]);
       DebugPutChar("LLLLHHHH"[which]);
    }
    else
    {
       DebugPutChar("ACDBSBSD"[which]);
       DebugPutChar("XXXXPPII"[which]);
    }

}


short do_sib(StackFrame *stackFrame, short m, long *deref)
{

    long sib_val, istr_val, factor, offset = 0;
    short pick_signed = DefaultPickSign;
    short s, i, b, extra=0;

    s = ss(sib());
    i = indx(sib());
    b = base(sib());
    if (b == 5)
    {
       if (m == 0)
       {
	  if (deref)
	     *deref = 0;
	  ProcessInstruction(stackFrame, "%p:[");
	  OutputHex('d', pSize, 0, AddressSize, 1, deref);
       }
       else
       {

	  if (debug_deref && stackFrame)
	     DebugPrint("<EBP=%08X>", stackFrame->tEBP);

	  if (deref && stackFrame)
	     *deref += stackFrame->tEBP;
	  ProcessInstruction(stackFrame, "%p:[EBP");
	  pick_signed |= 2;
       }
    }
    else
    {
       pick_signed |= 2;
       sib_val = sib_get_value(b, stackFrame);

       if (debug_deref)
	  DebugPrint("<sib=%08X>", sib_val);

       if (deref)
	  *deref += sib_val;

       ProcessInstruction(stackFrame, sib_str[b]);
       if ((b == i) && (b != 4) && (i != 5))
	  extra = 1;
    }
    if (extra == 0)
    {
       pick_signed |= 2;
       istr_val = istr_get_value(i, stackFrame);

       if (debug_deref)
	  DebugPrint("<istr=%08X>", istr_val);

       offset += istr_val;

       DebugPrint(i_str[i]);
    }

    if (i != 4 && s)
    {
       DebugPrint("*%X", (1 << s) + extra);
       factor = (1 << s) + extra;

       if (debug_deref)
	  DebugPrint("<factor=%08X>", factor);

       offset = (offset * factor);
    }

    if (deref)
       *deref += offset;

    return pick_signed;

}


void do_modrm(StackFrame *stackFrame, char t)
{

    long reg_val;
    short m = mod(modrm());
    short r = rm(modrm());
    short extend = (AddressSize == 32) ? 4 : 2;
    short pick_signed = DefaultPickSign;

    if (t == 'b')
       pSize = 1;
    else
    if (t == 'v' && OperandSize == 32)
       pSize = 4;
    else
    if (t == 'v' && OperandSize == 16)
       pSize = 2;

    if (m == 3)
    {
       reg_name(r, t);
       return;
    }
    if ((m == 0) && (r == 5) && (AddressSize == 32))
    {
       DeRefValue = 0;
       ProcessInstruction(stackFrame, "%p:[");
       OutputHex('d', extend, 0, AddressSize, 0, (long *) &DeRefValue);
       DebugPutChar(']');
       dereference_address(DeRefValue, pSize);
       return;
    }
    if ((m == 0) && (r == 6) && (AddressSize == 16))
    {
       DeRefValue = 0;
       ProcessInstruction(stackFrame, "%p:[");
       OutputHex('w', extend, 0, AddressSize, 0, (long *) &DeRefValue);
       DebugPutChar(']');
       dereference_address(DeRefValue, pSize);
       return;
    }
    if ((AddressSize != 32) || (r != 4))
    {
       DeRefValue = 0;
       ProcessInstruction(stackFrame, "%p:[");
    }

    if (AddressSize == 16)
    {
       DebugPrint(r_str[r]);
       pick_signed |= 2;
    }
    else
    {
       DeRefValue = 0;
       if (r == 4)
	  pick_signed |= do_sib(stackFrame, m, (long *)&DeRefValue);
       else
       {
	  reg_val = reg_get_value(r, stackFrame);
	  DeRefValue += reg_val;

	  if (debug_deref)
	     DebugPrint("<%s=%08X>", reg_names[2][r], reg_val);

	  DebugPrint(reg_names[2][r]);
	  pick_signed |= 2;
       }
    }
    MODRMExtend = extend;
    OutputHex("xbv"[m], extend, 1, AddressSize, pick_signed, (long *) &DeRefValue);
    DebugPutChar(']');
    dereference_address(DeRefValue, pSize);

}


void floating_point(StackFrame *stackFrame, short e1)
{

    short esc = e1 * 8 + reg(modrm());

    if (mod(modrm()) == 3)
    {
       if (fspecial[esc])
       {
	  if (fspecial[esc][0] && (fspecial[esc][0][0] == '*'))
	  {
	     ProcessInstruction(stackFrame, fspecial[esc][0]+1);
	  }
	  else
	  {
	     ProcessInstruction(stackFrame, fspecial[esc][rm(modrm())]);
	  }
       }
       else
       {
	  ProcessInstruction(stackFrame, floatops[esc]);
	  ProcessInstruction(stackFrame, " %EF");
       }
    }
    else
    {
       ProcessInstruction(stackFrame, floatops[esc]);
       ProcessInstruction(stackFrame, " %EF");
    }
}


void percent(StackFrame *stackFrame, char c, unsigned char **tptr)
{

     long vofs, delta;
     unsigned char *name;
     short default_signed = DefaultPickSign;
     char t = *(*tptr)++, it;
     short extend = (AddressSize == 32) ? 4 : 2;
     short iextend;

     if (c != '+')
     {
	if (t == '-')
	{
	   default_signed = 1;
	   t = *(*tptr)++;
	}
	else if (t == '+')
	{
	   default_signed = 2;
	   t = *(*tptr)++;
	}
     }
     switch (c)
     {

	case 'A':
	   OutputHex(t, extend, 0, AddressSize, 0, 0);
	   break;

	case 'C':
	   DebugPrint("CR%d", reg(modrm()));
	   break;

	case 'D':
	   DebugPrint("DR%d", reg(modrm()));
	   break;

	case 'E':
	   do_modrm(stackFrame, t);
	   break;

	case 'G':
	   if (t == 'F')
	      reg_name(rm(modrm()), t);
	   else
	      reg_name(reg(modrm()), t);
	   break;

	case 'I':
	   switch (t)
	   {
	      case 'b':
		 iextend = 1;
		 it = *(*tptr)++;
		 OutputHex(it, iextend, 0, OperandSize, default_signed, 0);
		 break;

	      case 'v':
		 iextend = extend;
		 it = *(*tptr)++;
		 OutputHex(it, iextend, 0, OperandSize, default_signed, 0);
		 break;

	      default:
		 iextend = 0;
		 OutputHex(t, iextend, 0, OperandSize, default_signed, 0);
		 break;
	   }
	   break;

	case 'J':
	   switch (bytes(t))
	   {

	      case 1:
		 vofs = (char) getbyte();
		 break;

	      case 2:
		 vofs = getbyte();
		 vofs += getbyte() << 8;
		 vofs = (int) vofs;
		 break;

	      case 4:
		 vofs = (long)getbyte();
		 vofs |= (long)getbyte() << 8;
		 vofs |= (long)getbyte() << 16;
		 vofs |= (long)getbyte() << 24;
		 break;

	      default:
		 vofs = 0;	/* To avoid uninit error */
	   }

	   name = output_jmp_address(stackFrame, vofs, (long *) &delta);
	   DebugPrint("%s", name);

	   if (delta)
	      DebugPrint("+%X (%X %c)", delta, ((long)vofs +
                        (long)VirtualAddress),
		        (vofs & 0x80000000UL) ? 0x1e : 0x1f);
	   break;

	case 'M':
	   do_modrm(stackFrame, t);
	   break;

	case 'O':
	   if (t == 'b')
	      pSize = 1;
	   else
	   if (t == 'v' && OperandSize == 32)
	      pSize = 4;
	   else
	   if (t == 'v' && OperandSize == 16)
	      pSize = 2;
	   DeRefValue = 0;
	   ProcessInstruction(stackFrame, "%p:[");
	   OutputHex(t, extend, 0, AddressSize, 0, (long *) &DeRefValue);
	   DebugPutChar(']');
	   dereference_address(DeRefValue, pSize);
	   break;

	case 'R':
	   do_modrm(stackFrame, t);
	   break;

	case 'S':
	   DebugPutChar("ECSDFG"[reg(modrm())]);
	   DebugPutChar('S');
	   break;

	case 'T':
	   DebugPrint("TR%d", reg(modrm()));
	   break;

	case 'X':
	   if (t == 'b')
	      pSize = 1;
	   else
	   if (t == 'v' && OperandSize == 32)
	      pSize = 4;
	   else
	   if (t == 'v' && OperandSize == 16)
	      pSize = 2;
	   DebugPrint("DS:[");
	   if (AddressSize == 32)
	      DebugPutChar('E');
	   DebugPrint("SI");
	   if (AddressSize==32 && stackFrame)
	      DebugPrint("=%08X", stackFrame->tESI);
	   DebugPutChar(']');

	   if (debug_deref && stackFrame)
	      DebugPrint("<ESI=%08X>", stackFrame->tESI);

           if (stackFrame)
	      dereference_address(stackFrame->tESI, pSize);
	   break;

	case 'Y':
	   if (t == 'b')
	      pSize = 1;
	   else
	   if (t == 'v' && OperandSize == 32)
	      pSize = 4;
	   else
	   if (t == 'v' && OperandSize == 16)
	      pSize = 2;
	   DebugPrint("ES:[");
	   if (AddressSize == 32)
	      DebugPutChar('E');
	   DebugPrint("DI");
	   if (AddressSize==32 && stackFrame)
	      DebugPrint("=%08X", stackFrame->tEDI);
	   DebugPutChar(']');

	   if (debug_deref && stackFrame)
	      DebugPrint("<EDI=%08X>", stackFrame->tEDI);

           if (stackFrame)
	      dereference_address(stackFrame->tEDI, pSize);
	   break;

	case '2':
	   ProcessInstruction(stackFrame, second[getbyte()]);
	   break;

	case 'e':
	   if (OperandSize == 32)
	   {
	      if (t == 'w')
		 DebugPutChar('D');
	      else
	      {
		 DebugPutChar('E');
		 DebugPutChar(toupper(t));
	      }
	   }
	   else
	      DebugPutChar(toupper(t));
	   break;

	case 'f':
	   floating_point(stackFrame, t - '0');
	   break;

	case 'g':
	   ProcessInstruction(stackFrame, groups[t - '0'][reg(modrm())]);
	   break;

	case 'p':
	   switch (t)
	   {
	      case 'c':
	      case 'd':
	      case 'e':
	      case 'f':
	      case 'g':
	      case 's':
		 Prefix = t;
		 ProcessInstruction(stackFrame, opmap1[getbyte()]);
		 break;

	      case ':':
		 if (Prefix)
		    DebugPrint("%cS:", toupper(Prefix));
		 break;

	      case ' ':
		 ProcessInstruction(stackFrame, opmap1[getbyte()]);
		 break;

	   }
	   break;

	case 's':
	   switch (t)
	   {
	      case 'a':
		 AddressSize = 48 - AddressSize;
		 ProcessInstruction(stackFrame, opmap1[getbyte()]);
		 break;

	      case 'o':
		 OperandSize = 48 - OperandSize;
		 ProcessInstruction(stackFrame, opmap1[getbyte()]);
		 break;
	   }
	   break;

	case '+':
	   switch (t)
	   {

	      case '-':
		 DefaultPickSign = 1;
		 break;

	      case '+':
		 DefaultPickSign = 2;
		 break;

	      default:
		 DefaultPickSign = 0;
		 break;
	   }
     }
}

unsigned long nestLevel;

void ProcessInstruction(StackFrame *stackFrame, unsigned char *s)
{

     short c;

     nestLevel++;
     if (nestLevel > 5)
     {
	DebugPrint("<INVALID OPCODE [NESTED]>");
	if (nestLevel)
	   nestLevel--;
	return;
     }

     if (s == 0)
     {
	DebugPrint("<INVALID OPCODE>");
	if (nestLevel)
	   nestLevel--;
	return;
     }

     while ((c = *s++) != 0)
     {
	if (c == '%')
	{
	   c = *s++;
	   percent(stackFrame, c, &s);
	}
	else
	if (c == ' ')
	   DebugPutChar('\t');
	else
	   DebugPutChar(c);
     }
     if (nestLevel)
	nestLevel--;

}

unsigned long unassemble(StackFrame *stackFrame, unsigned long ip,
                         unsigned long use, unsigned long *ret)
{

    long delta;
    long segment, offset;
    unsigned char *v = (unsigned char *) ip;

    if (use)
       SegmentSize = 32;
    else
       SegmentSize = 16;

    DefaultPickSign = 0;
    needs_proceed = 0;
    jmp_active = 0;
    nestLevel = 0;
    pSize = 4;

    output_address((void *)v, (unsigned long *) &delta);

    if (SegmentSize == 32)
    {
       DBGPrint("%08X ", (unsigned)v);
    }
    else
    {
       segment = (((unsigned long)v >> 4) & 0x0000FFFF);
       offset = ((unsigned long)v & 0x0000000F);
       DBGPrint("%04X:%04X ", (unsigned)segment, (unsigned)offset);
    }

    Prefix = 0;
    modrmv = sibv = -1;
    OperandSize = AddressSize = SegmentSize;
    VirtualAddress = (void *)v;
    bufp = bufe = 0;
    Columns = 0;
    ubufp = ubuf;
    ProcessInstruction(stackFrame, opmap1[getbyte()]);

    do
    {
       DBGPrint(" ");
       Columns++;
    } while (Columns < 15);

    Columns += strlen(ubuf);

    do
    {
       DebugPutChar(' ');
       Columns++;
    } while (Columns < 43);

    if (ret)
       *ret = (unsigned long)VirtualAddress;

    if (DBGPrint("%s\n", ubuf)) return 1;

    /*
    *  check for CALL, REP, INT, and LOOP instructions for proceed
    *  breakpoint
    */

    if ((strncmp(ubuf, "CALL", 4) == 0) || (strncmp(ubuf, "REP", 3) == 0) ||
	(strncmp(ubuf, "INT", 3) == 0)  || (strncmp(ubuf, "LOOP", 4) == 0))
    {
       needs_proceed = 1;
       jmp_active = 1;
    }

    if ((strncmp(ubuf, "IRET", 4) == 0) || (strncmp(ubuf, "RET", 3) == 0))
       jmp_active = 1;

    return (unsigned long) 0;

}

int mdb_unasm_length(void *ip, int use)
{
    unsigned char *oldVAddress;
    unsigned char *v = (unsigned char *) ip;

    if (use)
       SegmentSize = 32;
    else
       SegmentSize = 16;

    DefaultPickSign = 0;
    needs_proceed = 0;
    jmp_active = 0;
    nestLevel = 0;
    pSize = 4;

    Prefix = 0;
    modrmv = sibv = -1;
    OperandSize = AddressSize = SegmentSize;
    oldVAddress = VirtualAddress = (void *)v;
    bufp = bufe = 0;
    Columns = 0;
    ubufp = ubuf;
    ProcessInstruction(NULL, opmap1[getbyte()]);

    return (int)((unsigned long)VirtualAddress - (unsigned long)oldVAddress);

}
