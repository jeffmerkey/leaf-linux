
/***************************************************************************
 *
 *   Copyright (c) 2000-2015 Jeff V. Merkey  All Rights Reserved.
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

#ifndef DIS_ASM_H
#define DIS_ASM_H

#define mach_i386_i386 1
#define mach_i386_i8086 2
#define mach_i386_i386_intel_syntax 3
#define mach_x86_64 64
#define mach_x86_64_intel_syntax 65

#ifndef ATTR_UNUSED
#define ATTR_UNUSED
#endif

#if IS_ENABLED(CONFIG_X86_64)
#define BFD64
#define mdb_sprintf_vma(s, x) sprintf(s, "%016lx", x)
#else
#define mdb_sprintf_vma(s, x) sprintf(s, "%08lx", x)
#endif

#define mdb_byte unsigned char
#define mdb_vma unsigned long
#define mdb_signed_vma long

struct disassemble_info {
	int (*fprintf_func)(void *, const char *, ...);
	void *stream;
	unsigned long mach;
	void *private_data;

	int (*read_memory_func)
		(mdb_vma memaddr, mdb_byte *myaddr, unsigned int length,
		 struct disassemble_info *info);

	void (*memory_error_func)
		(int status, mdb_vma memaddr,
		 struct disassemble_info *info);

	void (*print_address_func)
		(mdb_vma addr, struct disassemble_info *info);

	int (*symbol_at_address_func)
		(mdb_vma addr, struct disassemble_info *info);

	int bytes_per_line;
	char *disassembler_options;

};

extern unsigned long full_deref_toggle;

int print_insn_i386(mdb_vma, struct disassemble_info *, dbg_regs *);
int print_insn_i386_att(mdb_vma, struct disassemble_info *, dbg_regs *);
int print_insn_i386_intel(mdb_vma, struct disassemble_info *, dbg_regs *);

#endif
