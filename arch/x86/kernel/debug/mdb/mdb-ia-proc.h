
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

#ifndef _MDB_IA32_PROC_H
#define _MDB_IA32_PROC_H

#define EXT_NMI_PORT             0x0461
#define NMI_IO_PORT              0x0462
#define NMI_CONTROL_PORT         0x0C6E
#define NMI_PORT                 0x61
#define PIC1_DEBUG_MASK          0xFC
#define PIC2_DEBUG_MASK          0xFF
#define EXCEPTION_ENTRIES        19
#define RESUME                   0x00010000
#define NESTED_TASK              0x00004000
#define SINGLE_STEP              0x00000100
#define INVALID_EXPRESSION       0
#define NUMERIC_EXPRESSION       1
#define BOOLEAN_EXPRESSION       2

/* DR7 Breakpoint Type and Length Fields */

#define BREAK_EXECUTE     0
#define BREAK_WRITE       1
#define BREAK_IOPORT      2
#define BREAK_READWRITE   3
#define ONE_BYTE_FIELD    0
#define TWO_BYTE_FIELD    1
#define EIGHT_BYTE_FIELD  2
#define FOUR_BYTE_FIELD   3

/* DR7 Register */

#define L0_BIT   0x00000001
#define G0_BIT   0x00000002
#define L1_BIT   0x00000004
#define G1_BIT   0x00000008
#define L2_BIT   0x00000010
#define G2_BIT   0x00000020
#define L3_BIT   0x00000040
#define G3_BIT   0x00000080
#define LEXACT   0x00000100
#define GEXACT   0x00000200
#define GDETECT  0x00002000
#define DR7DEF   0x00000400

/* DR6 Register */

#define B0_BIT   0x00000001
#define B1_BIT   0x00000002
#define B2_BIT   0x00000004
#define B3_BIT   0x00000008

#define BD_BIT   0x00002000
#define BS_BIT   0x00004000
#define BT_BIT   0x00008000

/* Memory Type Range Registers (MTRR) */

#define MTRR_PHYS_BASE_0    0x200
#define MTRR_PHYS_MASK_0    0x201
#define MTRR_PHYS_BASE_1    0x202
#define MTRR_PHYS_MASK_1    0x203
#define MTRR_PHYS_BASE_2    0x204
#define MTRR_PHYS_MASK_2    0x205
#define MTRR_PHYS_BASE_3    0x206
#define MTRR_PHYS_MASK_3    0x207
#define MTRR_PHYS_BASE_4    0x208
#define MTRR_PHYS_MASK_4    0x209
#define MTRR_PHYS_BASE_5    0x20A
#define MTRR_PHYS_MASK_5    0x20B
#define MTRR_PHYS_BASE_6    0x20C
#define MTRR_PHYS_MASK_6    0x20D
#define MTRR_PHYS_BASE_7    0x20E
#define MTRR_PHYS_MASK_7    0x20F

/* IA32 flags settings */

#define   CF_FLAG      0x00000001
#define   PF_FLAG      0x00000004
#define   AF_FLAG      0x00000010
#define   ZF_FLAG      0x00000040
#define   SF_FLAG      0x00000080
#define   TF_FLAG      0x00000100  /* ss flag */
#define   IF_FLAG      0x00000200
#define   DF_FLAG      0x00000400
#define   OF_FLAG      0x00000800
#define   NT_FLAG      0x00004000
#define   RF_FLAG      0x00010000  /* resume flag */
#define   VM_FLAG      0x00020000
#define   AC_FLAG      0x00040000
#define   VIF_FLAG     0x00080000
#define   VIP_FLAG     0x00100000
#define   ID_FLAGS     0x00200000

#if IS_ENABLED(CONFIG_X86_64)

struct GATE64 {
	u16 offset_low;
	u16 segment;
	unsigned ist:3, zero0:5, type:5, dpl:2, p:1;
	u16 offset_middle;
	u32 offset_high;
	u32 zero1;
} __packed;

struct LDTTSS64 {
	u16 limit0;
	u16 base0;
	unsigned base1:8, type:5, dpl:2, p:1;
	unsigned limit1:4, zero0:3, g:1, base2:8;
	u32 base3;
	u32 zero1;
} __packed;

#define GDT       struct GATE64
#define IDT       struct GATE64
#define LDT       struct LDTTSS64
#define TSS       struct LDTTSS64
#define TSS_GATE  struct LDTTSS64

#else

struct DESC {
	union {
		struct	{
			unsigned int a;
			unsigned int b;
		};
		struct	{
			u16 limit0;
			u16 base0;
			unsigned base1:8, type:4, s:1, dpl:2, p:1;
			unsigned limit:4, avl:1, l:1, d:1, g:1, base2:8;
		};
	};
} __packed;

struct GDT32 {
	u16 limit;    /*	0xFFFF */
	u16 base_1;    /*  0 */
	u8  base_2;     /*	0 */
	u8  gdt_type;   /*	10010010b */
	u8  other_type; /*	11001111b */
	u8  base_3;     /*	0 */
} __packed;

struct IDT32 {
	u16 idt_low;     /*	0 */
	u16 idt_segment; /*	0x08 */
	u8  idt_skip;     /*	0 */
	u8  idt_flags;    /*	10001110b */
	u16 idt_high;    /*	0 */
} __packed;

struct TSS32 {
	u16 tss_limit;	/* 0x0080 */
	u16 tss_base_1;	/* 0 */
	u8  tss_base_2;	/* 0 */
	u8  tss_type;	/* 10001001b */
	u8  tss_other_type;	/* 00000000b */
	u8  tss_base_3;	/* 0 */
} __packed;

struct TSS_GATE32 {
	u16 tss_res_1;	/* 0 */
	u16 tss_selector;	/* 0 */
	u8  tss_res_2;	/* 0 */
	u8  tss_flags;	/* 10000101b */
	u16 tss_res_3;	/* 0 */
} __packed;

struct LDT32 {
	u16 ldt_limit;	/* 0xFFFF */
	u16 ldt_base_1;	/* 0 */
	u8  ldt_base_2;	/* 0 */
	u8  ldt_gdt_type;	/* 10000010b */
	u8  ldt_other_type;	/* 10001111b */
	u8  ldt_base_3;	/* 0 */
} __packed;

#define GDT       struct GDT32
#define LDT       struct LDT32
#define IDT       struct IDT32
#define TSS       struct TSS32
#define TSS_GATE  struct TSS_GATE32

#endif

void initialize_debugger_registers(void);
unsigned long add_accel_routine(accel_key *);
void clear_debugger_registers(void);
void display_expr_help(void);
void eval_command_expr(dbg_regs *dbgframe,
		       unsigned char *cmd);
unsigned long cpu_mttr_on(void);
void hw_breakpoint_enable(void);

extern unsigned long curr_dr6;
extern unsigned long curr_dr7;

unsigned long is_processor_held(unsigned long cpu);
unsigned long dbg_read_dr0(void);
unsigned long dbg_read_dr1(void);
unsigned long dbg_read_dr2(void);
unsigned long dbg_read_dr3(void);
unsigned long dbg_read_dr6(void);
unsigned long dbg_read_dr7(void);
void dbg_write_dr0(unsigned long);
void dbg_write_dr1(unsigned long);
void dbg_write_dr2(unsigned long);
void dbg_write_dr3(unsigned long);
void dbg_write_dr6(unsigned long);
void dbg_write_dr7(unsigned long);
unsigned long dbg_read_cr0(void);
unsigned long dbg_read_cr2(void);
unsigned long dbg_read_cr3(void);
unsigned long dbg_read_cr4(void);
void read_gdtr(unsigned long *);
void read_idtr(unsigned long *);
unsigned long read_ldtr(void);
unsigned long read_tr(void);

void read_msr(unsigned long msr,
	      unsigned long *val1,
	      unsigned long *val2);
void write_msr(unsigned long msr,
	       unsigned long *val1,
	       unsigned long *val2);
void save_npx(NUMERIC_FRAME *npx);
void load_npx(NUMERIC_FRAME *npx);

unsigned long get_processor_id(void);
unsigned long get_physical_processor(void);
unsigned long fpu_present(void);

void read_task_frame(dbg_regs *sf,
		     struct task_struct *p);
void display_tss(dbg_regs *dbgframe);
void display_general_registers(dbg_regs *dbgframe);
void display_segment_registers(dbg_regs *dbgframe);
void display_control_registers(unsigned long processor,
			       dbg_regs *dbgframe);
double ldexp(double v, int e);
void display_npx_registers(dbg_regs *dbgframe);

unsigned long process_proceed_acc(unsigned long key,
				  void *dbgframe,
				  accel_key *accel);
unsigned long process_trace_acc(unsigned long key,
				void *dbgframe,
				accel_key *accel);
unsigned long process_trace_ssb_acc(unsigned long key,
				    void *dbgframe,
				    accel_key *accel);
unsigned long process_go_acc(unsigned long key,
			     void *dbgframe,
			     accel_key *accel);

unsigned long execute_command_help(unsigned char *command_line,
				   dbg_parser *parser);
unsigned long process_proceed(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long process_trace(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser);
unsigned long process_trace_ssb(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long process_go(unsigned char *cmd,
			 dbg_regs *dbgframe,
			 unsigned long exception,
			 dbg_parser *parser);

unsigned long processor_command_help(unsigned char *command_line,
				     dbg_parser *parser);
unsigned long break_processor(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long tss_display_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long tss_display(unsigned char *cmd,
			  dbg_regs *dbgframe,
			  unsigned long exception,
			  dbg_parser *parser);

unsigned long display_eax_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_eax_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long change_origeax_register(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser);

unsigned long display_ebx_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_ebx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_ecx_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_ecx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_edx_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_edx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_esi_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_esi_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_edi_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_edi_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_ebp_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_ebp_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_esp_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_esp_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_eip_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_eip_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_cs_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_cs_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long display_ds_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_ds_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long display_es_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_es_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long display_fs_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_fs_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long display_gs_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_gs_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long display_ss_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_ss_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long display_rf_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_rf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_tf_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_tf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_zf_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_zf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_sf_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_sf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_pf_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_pf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_cf_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_cf_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_of_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_of_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_if_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_if_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_id_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_id_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_df_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_df_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_nt_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_nt_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_vm_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_vm_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_vif_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_vif_flag(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);

unsigned long display_vip_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_vip_flag(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);

unsigned long display_af_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_af_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_ac_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_ac_flag(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);

unsigned long display_mtrr_help(unsigned char *command_line,
				dbg_parser *parser);
unsigned long show_mtrr_registers(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_gdt_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long show_gdt(unsigned char *cmd,
		       dbg_regs *dbgframe,
		       unsigned long exception,
		       dbg_parser *parser);

unsigned long display_idt_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long show_idt(unsigned char *cmd,
		       dbg_regs *dbgframe,
		       unsigned long exception,
		       dbg_parser *parser);

unsigned long evaluate_expr_help(unsigned char *command_line,
				 dbg_parser *parser);
unsigned long evaluate_expr(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser);

unsigned long display_d_os_table_help(unsigned char *command_line,
				      dbg_parser *parser);
unsigned long display_d_os_table(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);

unsigned long port_command_help(unsigned char *command_line,
				dbg_parser *parser);
unsigned long input_word_port(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long input_double_port(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long input_byte_port(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long input_port(unsigned char *cmd,
			 dbg_regs *dbgframe,
			 unsigned long exception,
			 dbg_parser *parser);
unsigned long output_word_port(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long output_double_port(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);
unsigned long output_byte_port(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long output_port(unsigned char *cmd,
			  dbg_regs *dbgframe,
			  unsigned long exception,
			  dbg_parser *parser);

unsigned long breakpoint_command_help(unsigned char *command_line,
				      dbg_parser *parser);
unsigned long breakpoint_clear_all(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser);
unsigned long breakpoint_clear(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long breakpoint_mask(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long breakpoint_word1(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long breakpoint_word2(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long breakpoint_word4(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
#if IS_ENABLED(CONFIG_X86_64)
unsigned long breakpoint_word8(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
#endif
unsigned long breakpoint_word(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long breakpoint_read1(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long breakpoint_read2(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
unsigned long breakpoint_read4(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
#if IS_ENABLED(CONFIG_X86_64)
unsigned long breakpoint_read8(unsigned char *cmd,
			       dbg_regs *dbgframe,
			       unsigned long exception,
			       dbg_parser *parser);
#endif
unsigned long breakpoint_read(unsigned char *cmd,
			      dbg_regs *dbgframe,
			      unsigned long exception,
			      dbg_parser *parser);
unsigned long breakpoint_io1(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);
unsigned long breakpoint_io2(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);
unsigned long breakpoint_io4(unsigned char *cmd,
			     dbg_regs *dbgframe,
			     unsigned long exception,
			     dbg_parser *parser);
unsigned long breakpoint_io(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser);
unsigned long breakpoint_execute(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);
unsigned long breakpoint_show_temp(unsigned char *cmd,
				   dbg_regs *dbgframe,
				   unsigned long exception,
				   dbg_parser *parser);
void mdb_breakpoint(void);

#if IS_ENABLED(CONFIG_SMP)
unsigned long display_apic_help(unsigned char *command_line,
				dbg_parser *parser);
unsigned long display_apic_info(unsigned char *cmd,
				dbg_regs *dbgframe,
				unsigned long exception,
				dbg_parser *parser);
unsigned long display_ioapic_info(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long nmi_processor(unsigned char *cmd,
			    dbg_regs *dbgframe,
			    unsigned long exception,
			    dbg_parser *parser);
#endif

#if IS_ENABLED(CONFIG_X86_64)
unsigned long display_rax_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rax_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long change_origrax_register(unsigned char *cmd,
				      dbg_regs *dbgframe,
				      unsigned long exception,
				      dbg_parser *parser);

unsigned long display_rbx_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rbx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_rcx_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rcx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_rdx_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rdx_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_rsi_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rsi_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_rdi_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rdi_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_rbp_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rbp_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_rsp_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rsp_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_rip_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_rip_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);

unsigned long display_r8_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_r8_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);
unsigned long display_r9_help(unsigned char *command_line,
			      dbg_parser *parser);
unsigned long change_r9_register(unsigned char *cmd,
				 dbg_regs *dbgframe,
				 unsigned long exception,
				 dbg_parser *parser);
unsigned long display_r10_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_r10_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long display_r11_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_r11_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long display_r12_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_r12_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long display_r13_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_r13_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long display_r14_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_r14_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
unsigned long display_r15_help(unsigned char *command_line,
			       dbg_parser *parser);
unsigned long change_r15_register(unsigned char *cmd,
				  dbg_regs *dbgframe,
				  unsigned long exception,
				  dbg_parser *parser);
#endif

#endif
