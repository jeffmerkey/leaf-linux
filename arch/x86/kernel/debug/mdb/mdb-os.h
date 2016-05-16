
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

#ifndef _MDB_OS_H
#define _MDB_OS_H

#define MAX_SYMBOL_LEN  (KSYM_NAME_LEN + 1)

extern atomic_t inmdb;
extern int pause_mode;
extern struct task_struct *mdb_current_task;
extern unsigned char *mdb_oops;
extern unsigned char *last_mdb_oops;
extern unsigned char symbuf[MAX_SYMBOL_LEN];
extern unsigned char modbuf[MAX_SYMBOL_LEN];
extern unsigned char workbuf[MAX_SYMBOL_LEN];
extern unsigned char delim_table[256];

unsigned long *in_exception_stack(unsigned int cpu, unsigned long stack,
				  unsigned int *usedp, char **idp);
unsigned long *get_irq_stack_end(const unsigned int cpu);
void *is_hardirq_stack(unsigned long *stack, int cpu);
void *is_softirq_stack(unsigned long *stack, int cpu);
unsigned long debug_rlock(spinlock_t *lock, rlock_t *rlock,
			  unsigned long p);
void debug_unrlock(spinlock_t *lock, rlock_t *rlock, unsigned long p);
#if IS_ENABLED(CONFIG_VT_CONSOLE) && IS_ENABLED(CONFIG_MDB_CONSOLE_REDIRECTION)
int vt_kmsg_redirect(int console);
#endif
void mdb_watchdogs(void);

unsigned long mdb_kallsyms_lookup_name(char *str);
int mdb_kallsyms(char *str,
		 int (*print)(char *s, ...));
int mdb_modules(char *str,
		int (*print)(char *s, ...));
int mdb_getkey(void);
int mdb_getlword(u64 *word,
		 unsigned long addr,
		 size_t size);
int mdb_putword(unsigned long addr,
		unsigned long word,
		size_t size);
int mdb_copy(void *to, void *from,
	     size_t size);
unsigned long mdb_phys_getword(unsigned long addr,
			       size_t size);
unsigned long mdb_getword(unsigned long addr,
			  size_t size);
u64 mdb_getqword(u64 *addr, size_t size);
u64 mdb_phys_getqword(u64 *addr, size_t size);
int mdb_putqword(u64 *addr, u64 word,
		 size_t size);
unsigned long mdb_segment_getword(unsigned long seg,
				  unsigned long addr,
				  size_t size);
u64 mdb_segment_getqword(unsigned long segment,
			 u64 *addr,
			 size_t size);
int mdb_verify_rw(void *addr, size_t size);
int closest_symbol(unsigned long address);
void dump_os_symbol_table_match(unsigned char *symbol);
void dump_os_symbol_table(void);
unsigned long get_value_from_symbol(unsigned char *symbol);
unsigned char *get_module_symbol_value(unsigned long value,
				       unsigned char *buf,
				       unsigned long len);
unsigned char *get_symbol_value(unsigned long value,
				unsigned char *buf,
				unsigned long len);
unsigned char *get_symbol_value_offset(unsigned long value,
				       unsigned long *sym_offset,
				       unsigned char *buf,
				       unsigned long len);
unsigned char *get_symbol_value_offset_module(unsigned long value,
					      unsigned long *sym_offset,
					      unsigned char **module,
					      unsigned char *buf,
					      unsigned long len);
unsigned long get_processor_id(void);
unsigned long get_physical_processor(void);
unsigned long fpu_present(void);
unsigned long cpu_mttr_on(void);
unsigned char *upcase_string(unsigned char *s);
void clear_screen(void);
unsigned long read_ds(void);
unsigned long read_es(void);
unsigned long read_fs(void);
unsigned long read_gs(void);
unsigned long dbg_read_dr(unsigned long regnum);
void dbg_write_dr(int regnum, unsigned long contents);
unsigned long dbg_read_cr(int regnum);
void dbg_write_cr(int regnum, unsigned long contents);
unsigned long read_tr(void);
unsigned long read_ldtr(void);
void read_gdtr(unsigned long *v);
void read_idtr(unsigned long *v);
void save_npx(NUMERIC_FRAME *v);
void load_npx(NUMERIC_FRAME *v);
unsigned long dbg_read_dr0(void);
unsigned long dbg_read_dr1(void);
unsigned long dbg_read_dr2(void);
unsigned long dbg_read_dr3(void);
unsigned long dbg_read_dr6(void);
unsigned long dbg_read_dr7(void);
void dbg_write_dr0(unsigned long v);
void dbg_write_dr1(unsigned long v);
void dbg_write_dr2(unsigned long v);
void dbg_write_dr3(unsigned long v);
void dbg_write_dr6(unsigned long v);
void dbg_write_dr7(unsigned long v);
unsigned long dbg_read_cr0(void);
unsigned long dbg_read_cr2(void);
unsigned long dbg_read_cr3(void);
unsigned long dbg_read_cr4(void);
void dbg_write_cr0(unsigned long v);
void dbg_write_cr2(unsigned long v);
void dbg_write_cr3(unsigned long v);
void dbg_write_cr4(unsigned long v);
void read_msr(unsigned long r, unsigned long *v1,
	      unsigned long *v2);
void write_msr(unsigned long r, unsigned long *v1,
	       unsigned long *v2);
#endif
