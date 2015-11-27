
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

#ifndef _MDB_OS_H
#define _MDB_OS_H

#define MAX_SYMBOL_LEN  KSYM_NAME_LEN+1

extern atomic_t inmdb;
extern int pause_mode;
extern struct task_struct *mdb_current_task;
extern unsigned char *mdb_oops;
extern unsigned char *last_mdb_oops;
extern unsigned char symbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];
extern unsigned char modbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];
extern unsigned char workbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];

extern int mdb_kallsyms(char *str, int (*print)(char *s, ...));
extern int mdb_modules(char *str, int (*print)(char *s, ...));
extern int mdb_getkey(void);
extern int mdb_getlword(uint64_t *word, unsigned long addr, size_t size);
extern int mdb_putword(unsigned long addr, unsigned long word, size_t size);
extern int mdb_copy(void *to, void *from, size_t size);
extern unsigned long mdb_getword(unsigned long addr, size_t size);
extern uint64_t mdb_getqword(uint64_t *addr, size_t size);
extern int mdb_putqword(uint64_t *addr, uint64_t word, size_t size);
extern unsigned long mdb_segment_getword(unsigned long seg, 
                                         unsigned long addr, size_t size);
extern uint64_t mdb_segment_getqword(unsigned long segment,
                                     uint64_t *addr, size_t size);
extern int mdb_verify_rw(void *addr, size_t size);
extern unsigned long ValidateAddress(unsigned long addr, unsigned long length);
extern int DisplayClosestSymbol(unsigned long address);
extern void DumpOSSymbolTableMatch(unsigned char *symbol);
extern void DumpOSSymbolTable(void);
extern unsigned long GetValueFromSymbol(unsigned char *symbol);
extern unsigned char *GetModuleInfoFromSymbolValue(unsigned long value, unsigned char *buf, unsigned long len);
extern unsigned char *GetSymbolFromValue(unsigned long value, unsigned char *buf, unsigned long len);
extern unsigned char *GetSymbolFromValueWithOffset(unsigned long value, unsigned long *sym_offset,
                                          unsigned char *buf, unsigned long len);
extern unsigned char *GetSymbolFromValueOffsetModule(unsigned long value, unsigned long *sym_offset,
                                     unsigned char **module, unsigned char *buf, unsigned long len);
extern unsigned long get_processor_id(void);
extern unsigned long get_physical_processor(void);
extern unsigned long fpu_present(void);
extern unsigned long cpu_mttr_on(void);
extern unsigned char *UpcaseString(unsigned char *s);
extern void ClearScreen(void);
extern unsigned long ReadDS(void);
extern unsigned long ReadES(void);
extern unsigned long ReadFS(void);
extern unsigned long ReadGS(void);
extern unsigned long ReadDR(unsigned long regnum);
extern void WriteDR(int regnum, unsigned long contents);
extern unsigned long ReadCR(int regnum);
extern void WriteCR(int regnum, unsigned long contents);
extern unsigned long ReadTR(void);
extern unsigned long ReadLDTR(void);
extern void ReadGDTR(unsigned long *v);
extern void ReadIDTR(unsigned long *v);
extern void save_npx(NUMERIC_FRAME *v);
extern void load_npx(NUMERIC_FRAME *v);
extern unsigned long ReadDR0(void);
extern unsigned long ReadDR1(void);
extern unsigned long ReadDR2(void);
extern unsigned long ReadDR3(void);
extern unsigned long ReadDR6(void);
extern unsigned long ReadDR7(void);
extern void WriteDR0(unsigned long v);
extern void WriteDR1(unsigned long v);
extern void WriteDR2(unsigned long v);
extern void WriteDR3(unsigned long v);
extern void WriteDR6(unsigned long v);
extern void WriteDR7(unsigned long v);
extern unsigned long ReadCR0(void);
extern unsigned long ReadCR2(void);
extern unsigned long ReadCR3(void);
extern unsigned long ReadCR4(void);
extern void WriteCR0(unsigned long v);
extern void WriteCR2(unsigned long v);
extern void WriteCR3(unsigned long v);
extern void WriteCR4(unsigned long v);
extern void ReadMSR(unsigned long r, unsigned long *v1, unsigned long *v2);
extern void WriteMSR(unsigned long r, unsigned long *v1, unsigned long *v2);
#endif
