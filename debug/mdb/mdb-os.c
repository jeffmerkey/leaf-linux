
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
#include <linux/nmi.h>
#include <asm/system.h>
#include <asm/processor.h>
#include <asm/segment.h>
#include <asm/atomic.h>
#include <asm/msr.h>
#include <asm-generic/kmap_types.h>
#include <linux/io.h>
#include <linux/delay.h>

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

/* module symbol workspace */
unsigned char symbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];
unsigned char modbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];
unsigned char workbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];
unsigned char traceBuf[MAX_SYMBOL_LEN];

#ifdef CONFIG_X86_64 

/* The form of the top of the frame on the stack */
struct stack_frame {
	struct stack_frame *next_frame;
	unsigned long return_address;
};

static inline int in_irq_stack(unsigned long *stack, unsigned long *irq_stack,
	     unsigned long *irq_stack_end)
{
	return (stack >= irq_stack && stack < irq_stack_end);
}

static inline unsigned long fixup_bp_irq_link(unsigned long bp, 
unsigned long *stack, unsigned long *irq_stack, unsigned long *irq_stack_end)
{
#ifdef CONFIG_FRAME_POINTER
	struct stack_frame *frame = (struct stack_frame *)bp;
	unsigned long next;

	if (!in_irq_stack(stack, irq_stack, irq_stack_end)) {
		if (!probe_kernel_address(&frame->next_frame, next))
			return next;
		else
			DBGPrint("MDB: bad frame pointer = %p in chain\n", 
                                 &frame->next_frame);
	}
#endif
	return bp;
}

#define get_bp(bp) asm("movq %%rbp, %0" : "=r" (bp) :)

extern unsigned long *in_exception_stack(unsigned cpu, unsigned long stack,
    			                 unsigned *usedp, char **idp);
extern unsigned long *get_irq_stack_end(const unsigned int cpu);

void DBGPrint_address(unsigned long address, int reliable)
{
	DBGPrint(" [<%p>] %s%pS\n", (void *) address,
		 reliable ? "" : "? ", (void *) address);
}

/*
 * x86-64 can have up to three kernel stacks:
 * process stack
 * interrupt stack
 * severe exception (double fault, nmi, stack fault, debug, mce) hardware stack
 */

static inline int valid_stack_ptr(struct thread_info *tinfo,
			void *p, unsigned int size, void *end)
{
	void *t = tinfo;
	if (end) {
		if (p < end && p >= (end-THREAD_SIZE))
			return 1;
		else
			return 0;
	}
	return p > t && p < t + THREAD_SIZE - size;
}

static inline unsigned long
print_context_stack(struct thread_info *tinfo, unsigned long *stack, 
                    unsigned long bp, unsigned long *end)
{
	struct stack_frame *frame = (struct stack_frame *)bp;

	while (valid_stack_ptr(tinfo, stack, sizeof(*stack), end)) {
		unsigned long addr;

		addr = *stack;
		if (__kernel_text_address(addr)) {
			if ((unsigned long) stack == bp + sizeof(long)) {
	                        touch_nmi_watchdog();
	                        DBGPrint_address(addr, 1);
				frame = frame->next_frame;
				bp = (unsigned long) frame;
			} else {
	                        touch_nmi_watchdog();
	                        DBGPrint_address(addr, bp == 0);
			}
		}
		stack++;
	}
	return bp;
}

void bt_stack(struct task_struct *task, struct pt_regs *regs,
	      unsigned long *stack)
{
        unsigned long bp = 0;
	const unsigned cpu = get_cpu();
	unsigned long *irq_stack_end = get_irq_stack_end(cpu);
	unsigned used = 0;
	struct thread_info *tinfo;

	if (!task)
		task = current;

	if (!stack) {
		unsigned long dummy;
		stack = &dummy;
		if (task && task != current)
			stack = (unsigned long *)task->thread.sp;
	}

#ifdef CONFIG_FRAME_POINTER
	if (!bp) 
        {
		if (task == current) 
                {
			/* Grab bp right from our regs */
			get_bp(bp);
		} 
                else 
                {
			/* bp is the last reg pushed by switch_to */
			bp = *(unsigned long *) task->thread.sp;
		}
	}
#endif

	/*
	 * Print function call entries in all stacks, starting at the
	 * current stack address. If the stacks consist of nested
	 * exceptions
	 */
	tinfo = task_thread_info(task);
	for (;;) 
        {
		char *id;
		unsigned long *estack_end;
		estack_end = in_exception_stack(cpu, (unsigned long)stack,
						&used, &id);

		if (estack_end) {
			if (DBGPrint("%s", id))
 			   break;

			bp = print_context_stack(tinfo, stack, bp, estack_end);
			DBGPrint("%s", "<EOE>");

			/*
			 * We link to the next stack via the
			 * second-to-last pointer (index -2 to end) in the
			 * exception stack:
			 */
			stack = (unsigned long *) estack_end[-2];
			continue;
		}

		if (irq_stack_end) 
                {
			unsigned long *irq_stack;
			irq_stack = irq_stack_end -
				(IRQ_STACK_SIZE - 64) / sizeof(*irq_stack);

			if (in_irq_stack(stack, irq_stack, irq_stack_end)) 
                        {
			      if (DBGPrint("%s", "IRQ"))
		 	         break;

			      bp = print_context_stack(tinfo, stack, bp,
			    		               irq_stack_end);
			      /*
			       * We link to the next stack (which would be
			       * the process stack normally) the last
			       * pointer (index -1 to end) in the IRQ stack:
			       */

			      stack = (unsigned long *) (irq_stack_end[-1]);
			      bp = fixup_bp_irq_link(bp, stack, irq_stack,
						       irq_stack_end);
			      irq_stack_end = NULL;
			      DBGPrint("%s", "EOI");
			      continue;
			}
		}
		break;
	}

	/*
	 * This handles the process stack:
	 */
	bp = print_context_stack(tinfo, stack, bp, NULL);
	put_cpu();
}

#else

int valid_stack_ptr(struct thread_info *tinfo, void *p)
{
    return ((p > (void *)tinfo) && (p < (void *)(tinfo + THREAD_SIZE - 3)));
}

unsigned long print_context_stack(struct thread_info *tinfo,
			  unsigned long *stack, unsigned long ebp,
                          unsigned long *rebp)
{
    register unsigned char *symbol;
    unsigned long addr, offset;
    unsigned char *modname;

#ifdef	CONFIG_FRAME_POINTER
    while (valid_stack_ptr(tinfo, (void *)ebp))
    {
	addr = mdb_getword((ebp + 4), 4);

        if (DBGPrint("[<%08lx>] ", addr))
           return 1;

        symbol = GetSymbolFromValueOffsetModule(addr, &offset, &modname,
                                                traceBuf, MAX_SYMBOL_LEN);
        if (symbol)
        {
           if (modname)
           {
              if (offset)
              {
                 if (DBGPrint("%s|%s+0x%X", modname, symbol, offset))
                    return 1;
              }
              else
              {
                 if (DBGPrint("%s|%s", modname, symbol))
                    return 1;
              }
           }
           else
           {
              if (offset)
              {
                 if (DBGPrint("%s+0x%X", symbol, offset))
                    return 1;
              }
              else
              {
                 if (DBGPrint("%s", symbol))
                    return 1;
              }
           }
        }
        if (DBGPrint("\n"))
           return 1;

	if (ebp == mdb_getword(ebp, 4))
	   break;

	ebp = mdb_getword(ebp, 4);
    }
#else
    while (valid_stack_ptr(tinfo, stack))
    {
       addr = mdb_getword((unsigned long)stack, 4);
       stack++;

       if (__kernel_text_address(addr))
       {
          if (DBGPrint("[<%08lx>] ", addr))
             return 1;

          symbol = GetSymbolFromValueOffsetModule(addr, &offset, &modname,
                                                traceBuf, MAX_SYMBOL_LEN);
          if (symbol)
          {
             if (modname)
             {
                if (offset)
                {
                   if (DBGPrint("%s|%s+0x%X", modname, symbol, offset))
                      return 1;
                }
                else
                {
                   if (DBGPrint("%s|%s", modname, symbol))
                      return 1;
                }
             }
             else
             {
                if (offset)
                {
                   if (DBGPrint("%s+0x%X", symbol, offset))
                      return 1;
                }
                else
                {
                   if (DBGPrint("%s", symbol))
                      return 1;
                }
             }
          }
          if (DBGPrint("\n"))
             return 1;
       }
    }
#endif
    if (rebp)
       *rebp = ebp;

    return 0;
}

int bt_stack(struct task_struct *task, struct pt_regs *regs,
             unsigned long *stack)
{
    unsigned long ebp = 0;

    if (!task)
       task = current;

    if (!stack)
    {
       unsigned long dummy;

       stack = &dummy;
       if (task && task != current)
       {
          if (mdb_verify_rw((void *)&task->thread.sp, 4))
             return 0;

          stack = (unsigned long *)
                  mdb_getword((unsigned long)&task->thread.sp, 4);
       }
    }

#ifdef CONFIG_FRAME_POINTER
    if (!ebp)
    {
       if (task == current)
          asm ("movl %%ebp, %0" : "=r" (ebp) : );
       else
       {
          if (mdb_verify_rw((void *)task->thread.sp, 4))
             return 0;

          ebp = (unsigned long)mdb_getword(task->thread.sp, 4);
       }
    }
#endif

    while (1)
    {
       struct thread_info *context;

       context = (struct thread_info *)
                 ((unsigned long)stack & (~(THREAD_SIZE - 1)));

       if (mdb_verify_rw(context, 4))
          return 0;

       if (print_context_stack(context, stack, ebp, &ebp))
          return 1;

       if (mdb_verify_rw(&context->previous_esp, 4))
          return 0;

       stack = (unsigned long *)
               mdb_getword((unsigned long)&context->previous_esp, 4);
       if (!stack)
          break;

       touch_nmi_watchdog();
    }
    return 0;
}
#endif

unsigned char *mdbprompt = "--- More (Q to Quit) ---";
int nextline;
int pause_mode = 1;

#if defined(CONFIG_SMP)
volatile rlock_t mdb_mutex = { SPIN_LOCK_UNLOCKED, -1, 0 };
#else
volatile rlock_t mdb_mutex = { -1, 0 };
#endif

extern unsigned long debug_rlock(volatile rlock_t *rlock, unsigned long p);
extern void debug_unrlock(volatile rlock_t *rlock, unsigned long p);
#ifdef CONFIG_MDB_CONSOLE_REDIRECTION
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
extern int vt_kmsg_redirect(int console);
#else
extern int kmsg_redirect;
#endif
#endif

char mdb_buffer[256];
char mdb_keystroke[16];
volatile unsigned long consolestate;

int mdb_printf(char *fmt, ...)
{
	va_list	ap;
	int linecount;
	struct console *con;
#ifdef CONFIG_MDB_CONSOLE_REDIRECTION
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
#else
       int kmsg_redirect_save;
#endif
#endif

        preempt_disable();
	debug_rlock(&mdb_mutex, get_processor_id());

#ifdef CONFIG_MDB_CONSOLE_REDIRECTION
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
        consolestate = vt_kmsg_redirect(-1);
        if (consolestate)
           consolestate = vt_kmsg_redirect(0);
#else
        kmsg_redirect_save = kmsg_redirect;
        kmsg_redirect = 0;
#endif
#endif
        linecount = 23;
	va_start(ap, fmt);
	vsprintf(mdb_buffer, fmt, ap);
	va_end(ap);

	for (con = console_drivers; con; con = con->next)
        {
	   if ((con->flags & CON_ENABLED) && con->write &&
	        (cpu_online(get_processor_id()) ||
		(con->flags & CON_ANYTIME)))
           {
	      con->write(con, mdb_buffer, strlen(mdb_buffer));
              touch_nmi_watchdog();
           }
	}

	if (strchr(mdb_buffer, '\n') != NULL)
	   nextline++;

	if (pause_mode && (nextline == linecount))
        {
	   nextline = 0;

	   for (con = console_drivers; con; con = con->next)
           {
	      if ((con->flags & CON_ENABLED) && con->write &&
	           (cpu_online(get_processor_id()) ||
	           (con->flags & CON_ANYTIME)))
              {
	         con->write(con, mdbprompt, strlen(mdbprompt));
                 touch_nmi_watchdog();
              }
	   }

	   mdb_keystroke[0] = (char)mdb_getkey();
	   nextline = 1;
	   if ((mdb_keystroke[0] == 'q') || (mdb_keystroke[0] == 'Q'))
           {
	      for (con = console_drivers; con; con = con->next)
              {
		 if ((con->flags & CON_ENABLED) && con->write &&
 	             (cpu_online(get_processor_id()) ||
		     (con->flags & CON_ANYTIME)))
                 {
		    con->write(con, "\n", 1);
                    touch_nmi_watchdog();
                 }
	      }
#ifdef CONFIG_MDB_CONSOLE_REDIRECTION
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
              if (consolestate)
                 vt_kmsg_redirect(consolestate);
#else
              kmsg_redirect = kmsg_redirect_save;
#endif
#endif
	      debug_unrlock(&mdb_mutex, get_processor_id());
              preempt_enable();
              return 1;
           }
	}
#ifdef CONFIG_MDB_CONSOLE_REDIRECTION
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
        if (consolestate)
           vt_kmsg_redirect(consolestate);
#else
        kmsg_redirect = kmsg_redirect_save;
#endif
#endif
	debug_unrlock(&mdb_mutex, get_processor_id());
        preempt_enable();
        return 0;
}


int mdb_serial_port;

int get_modem_char(void)
{
    unsigned char ch;
    int status;

    if (mdb_serial_port == 0)
       return -1;

    if ((status = inb(mdb_serial_port + UART_LSR)) & UART_LSR_DR)
    {
       ch = inb(mdb_serial_port + UART_RX);
       switch (ch)
       {
	   case 0x7f:
	      ch = 8;
              break;

           case '\t':
	      ch = ' ';
              break;

           case 8:  /* backspace */
              break;

	   case 13: /* enter */
	      DBGPrint("\n");
              break;

           default:
	      if (!isprint(ch))
	         return(-1);
	      DBGPrint("%c", ch);
              break;
	}
	return ch;
    }
    return -1;
}

#if defined(CONFIG_MDB_MODULE)
u_short plain_map[NR_KEYS] = {
	0xf200,	0xf01b,	0xf031,	0xf032,	0xf033,	0xf034,	0xf035,	0xf036,
	0xf037,	0xf038,	0xf039,	0xf030,	0xf02d,	0xf03d,	0xf07f,	0xf009,
	0xfb71,	0xfb77,	0xfb65,	0xfb72,	0xfb74,	0xfb79,	0xfb75,	0xfb69,
	0xfb6f,	0xfb70,	0xf05b,	0xf05d,	0xf201,	0xf702,	0xfb61,	0xfb73,
	0xfb64,	0xfb66,	0xfb67,	0xfb68,	0xfb6a,	0xfb6b,	0xfb6c,	0xf03b,
	0xf027,	0xf060,	0xf700,	0xf05c,	0xfb7a,	0xfb78,	0xfb63,	0xfb76,
	0xfb62,	0xfb6e,	0xfb6d,	0xf02c,	0xf02e,	0xf02f,	0xf700,	0xf30c,
	0xf703,	0xf020,	0xf207,	0xf100,	0xf101,	0xf102,	0xf103,	0xf104,
	0xf105,	0xf106,	0xf107,	0xf108,	0xf109,	0xf208,	0xf209,	0xf307,
	0xf308,	0xf309,	0xf30b,	0xf304,	0xf305,	0xf306,	0xf30a,	0xf301,
	0xf302,	0xf303,	0xf300,	0xf310,	0xf206,	0xf200,	0xf03c,	0xf10a,
	0xf10b,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,
	0xf30e,	0xf702,	0xf30d,	0xf01c,	0xf701,	0xf205,	0xf114,	0xf603,
	0xf118,	0xf601,	0xf602,	0xf117,	0xf600,	0xf119,	0xf115,	0xf116,
	0xf11a,	0xf10c,	0xf10d,	0xf11b,	0xf11c,	0xf110,	0xf311,	0xf11d,
	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,
};

u_short shift_map[NR_KEYS] = {
	0xf200,	0xf01b,	0xf021,	0xf040,	0xf023,	0xf024,	0xf025,	0xf05e,
	0xf026,	0xf02a,	0xf028,	0xf029,	0xf05f,	0xf02b,	0xf07f,	0xf009,
	0xfb51,	0xfb57,	0xfb45,	0xfb52,	0xfb54,	0xfb59,	0xfb55,	0xfb49,
	0xfb4f,	0xfb50,	0xf07b,	0xf07d,	0xf201,	0xf702,	0xfb41,	0xfb53,
	0xfb44,	0xfb46,	0xfb47,	0xfb48,	0xfb4a,	0xfb4b,	0xfb4c,	0xf03a,
	0xf022,	0xf07e,	0xf700,	0xf07c,	0xfb5a,	0xfb58,	0xfb43,	0xfb56,
	0xfb42,	0xfb4e,	0xfb4d,	0xf03c,	0xf03e,	0xf03f,	0xf700,	0xf30c,
	0xf703,	0xf020,	0xf207,	0xf10a,	0xf10b,	0xf10c,	0xf10d,	0xf10e,
	0xf10f,	0xf110,	0xf111,	0xf112,	0xf113,	0xf213,	0xf203,	0xf307,
	0xf308,	0xf309,	0xf30b,	0xf304,	0xf305,	0xf306,	0xf30a,	0xf301,
	0xf302,	0xf303,	0xf300,	0xf310,	0xf206,	0xf200,	0xf03e,	0xf10a,
	0xf10b,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,
	0xf30e,	0xf702,	0xf30d,	0xf200,	0xf701,	0xf205,	0xf114,	0xf603,
	0xf20b,	0xf601,	0xf602,	0xf117,	0xf600,	0xf20a,	0xf115,	0xf116,
	0xf11a,	0xf10c,	0xf10d,	0xf11b,	0xf11c,	0xf110,	0xf311,	0xf11d,
	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,
};

u_short ctrl_map[NR_KEYS] = {
	0xf200,	0xf200,	0xf200,	0xf000,	0xf01b,	0xf01c,	0xf01d,	0xf01e,
	0xf01f,	0xf07f,	0xf200,	0xf200,	0xf01f,	0xf200,	0xf008,	0xf200,
	0xf011,	0xf017,	0xf005,	0xf012,	0xf014,	0xf019,	0xf015,	0xf009,
	0xf00f,	0xf010,	0xf01b,	0xf01d,	0xf201,	0xf702,	0xf001,	0xf013,
	0xf004,	0xf006,	0xf007,	0xf008,	0xf00a,	0xf00b,	0xf00c,	0xf200,
	0xf007,	0xf000,	0xf700,	0xf01c,	0xf01a,	0xf018,	0xf003,	0xf016,
	0xf002,	0xf00e,	0xf00d,	0xf200,	0xf20e,	0xf07f,	0xf700,	0xf30c,
	0xf703,	0xf000,	0xf207,	0xf100,	0xf101,	0xf102,	0xf103,	0xf104,
	0xf105,	0xf106,	0xf107,	0xf108,	0xf109,	0xf208,	0xf204,	0xf307,
	0xf308,	0xf309,	0xf30b,	0xf304,	0xf305,	0xf306,	0xf30a,	0xf301,
	0xf302,	0xf303,	0xf300,	0xf310,	0xf206,	0xf200,	0xf200,	0xf10a,
	0xf10b,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,
	0xf30e,	0xf702,	0xf30d,	0xf01c,	0xf701,	0xf205,	0xf114,	0xf603,
	0xf118,	0xf601,	0xf602,	0xf117,	0xf600,	0xf119,	0xf115,	0xf116,
	0xf11a,	0xf10c,	0xf10d,	0xf11b,	0xf11c,	0xf110,	0xf311,	0xf11d,
	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,	0xf200,
};
#endif

void keyboard_reset(void)
{
	while (kbd_read_status() & KBD_STAT_IBF);
	kbd_write_command(KBD_CMD_RESET);
	mdelay(1);
	while (kbd_read_status() & KBD_STAT_IBF);
}

void keyboard_setup(void)
{
	int timeout;
	unsigned char c;

	while (kbd_read_status() & KBD_STAT_IBF);
	kbd_write_command(KBD_CCMD_READ_MODE);
	mdelay(1);
	while (kbd_read_status() & KBD_STAT_IBF);
	for (timeout = 200 * 1000; timeout &&
			(!(kbd_read_status() & KBD_STAT_OBF)); timeout--);
	c = kbd_read_input();
	c &= ~KBD_MODE_KBD_INT;
	while (kbd_read_status() & KBD_STAT_IBF);
	kbd_write_command(KBD_CCMD_WRITE_MODE);
	mdelay(1);
	while (kbd_read_status() & KBD_STAT_IBF);
	kbd_write_output(c);
	mdelay(1);
	while (kbd_read_status() & KBD_STAT_IBF);
	mdelay(1);
}

void keyboard_cleanup(void)
{
	int timeout;
	unsigned char c;

	while (kbd_read_status() & KBD_STAT_IBF);
	kbd_write_command(KBD_CCMD_READ_MODE);
	mdelay(1);
	while (kbd_read_status() & KBD_STAT_IBF);
	for (timeout = 200 * 1000; timeout &&
			(!(kbd_read_status() & KBD_STAT_OBF)); timeout--);
	c = kbd_read_input();
	c |= KBD_MODE_KBD_INT;
	while (kbd_read_status() & KBD_STAT_IBF);
	kbd_write_command(KBD_CCMD_WRITE_MODE);
	mdelay(1);
	while (kbd_read_status() & KBD_STAT_IBF);
	kbd_write_output(c);
	mdelay(1);
	while (kbd_read_status() & KBD_STAT_IBF);
	mdelay(1);
}

int mdb_suppress_crlf;
static int get_kbd_char(void)
{
	int	scancode, scanstatus;
	static int shift_lock;	/* CAPS LOCK state (0-off, 1-on) */
	static int shift_key;	/* Shift next keypress */
	static int ctrl_key;
	u_short keychar;
#if !defined(CONFIG_MDB_MODULE)
        extern u_short plain_map[], shift_map[], ctrl_map[];
#endif

	if ((inb(KBD_STATUS_REG) & KBD_STAT_OBF) == 0)
		return -1;
	/*
	 * Fetch the scancode
	 */
	scancode = inb(KBD_DATA_REG);
	scanstatus = inb(KBD_STATUS_REG);

	/*
	 * Ignore mouse events.
	 */
	if (scanstatus & KBD_STAT_MOUSE_OBF)
		return -1;

	/*
	 * Ignore release, trigger on make
	 * (except for shift keys, where we want to
	 *  keep the shift state so long as the key is
	 *  held down).
	 */

	if (((scancode & 0x7f) == 0x2a) ||
            ((scancode & 0x7f) == 0x36))
        {
		/*
		 * Next key may use shift table
		 */
		if ((scancode & 0x80) == 0) {
			shift_key=1;
		} else {
			shift_key=0;
		}
		return -1;
	}

	if ((scancode & 0x7f) == 0x1d) {
		/*
		 * Left ctrl key
		 */
		if ((scancode & 0x80) == 0) {
			ctrl_key = 1;
		} else {
			ctrl_key = 0;
		}
		return -1;
	}

	if ((scancode & 0x80) != 0)
		return -1;

	scancode &= 0x7f;

	/*
	 * Translate scancode
	 */

	if (scancode == 0x3a) {
		/*
		 * Toggle caps lock
		 */
		shift_lock ^= 1;
		return -1;
	}

	if (scancode == 0x0e) {
		/*
		 * Backspace
		 */
		return 8;
	}

	if (scancode == 0xe0) {
		return -1;
	}

	/*
	 * For Japanese 86/106 keyboards
	 *	See comment in drivers/char/pc_keyb.c.
	 *	- Masahiro Adegawa
	 */
	if (scancode == 0x73) {
		scancode = 0x59;
	} else if (scancode == 0x7d) {
		scancode = 0x7c;
	}

	if (!shift_lock && !shift_key && !ctrl_key) {
		keychar = plain_map[scancode];
	} else if (shift_lock || shift_key) {
		keychar = shift_map[scancode];
	} else if (ctrl_key) {
		keychar = ctrl_map[scancode];
	} else {
		keychar = 0x0020;
		DBGPrint("Unknown state/scancode (%d)\n", scancode);
	}

	keychar &= 0x0fff;
	if (keychar == '\t')
		keychar = ' ';

        switch (keychar)
        {
           case K_F1:
           case K_F2:
           case K_F3:
           case K_F4:
           case K_F5:
           case K_F6:
           case K_F7:
           case K_F8:
           case K_F9:
           case K_F10:
           case K_F11:
           case K_F12:
	      return keychar;
           default:
              break;
        }

	switch (KTYP(keychar))
        {
	   case KT_LETTER:
	   case KT_LATIN:
		if (isprint(keychar))
			break;		/* printable characters */
		/* drop through */
	   case KT_SPEC:
		if (keychar == K_ENTER)
			break;
		/* drop through */
           case KT_PAD:
                switch (keychar)
                {
                   case K_P0:
                   case K_P1:
                   case K_P2:
                   case K_P4:
                   case K_P6:
                   case K_P7:
                   case K_P8:
                   case K_PDOT:
                      return keychar;
                }
                return -1;

           case KT_CUR:
                switch (keychar)
                {
                   case K_DOWN:
                   case K_LEFT:
                   case K_RIGHT:
                   case K_UP:
                      return keychar;
                }
                return -1;

	   default:
		return(-1);	/* ignore unprintables */
	}

	if ((scancode & 0x7f) == 0x1c) {
		/*
		 * enter key.  All done.  Absorb the release scancode.
		 */
		while ((inb(KBD_STATUS_REG) & KBD_STAT_OBF) == 0)
			;

		/*
		 * Fetch the scancode
		 */
		scancode = inb(KBD_DATA_REG);
		scanstatus = inb(KBD_STATUS_REG);

		while (scanstatus & KBD_STAT_MOUSE_OBF)
                {
		   scancode = inb(KBD_DATA_REG);
		   scanstatus = inb(KBD_STATUS_REG);
		}

                /* enter-release error */
		if (scancode != 0x9c) {} ;

                if (!mdb_suppress_crlf)
		   DBGPrint("\n");
		return 13;
	}

	/*
	 * echo the character.
	 */
	DBGPrint("%c", keychar & 0xff);
	return keychar & 0xff;
}

int mdb_getkey(void)
{
   int key = -1;

   for (;;)
   {
      key = get_kbd_char();
      if (key != -1)
	 break;

      touch_nmi_watchdog();
   }
   return key;
}

struct task_struct *mdb_current_task = NULL;

static struct page *mdb_get_user_page(const struct task_struct *tsk, 
                                      void *to, int len, int write)
{
	struct mm_struct *mm = tsk->mm;
	unsigned int flags;
	struct vm_area_struct *	vma;
        unsigned long start = (unsigned long)to;

	if ((start & PAGE_MASK) != ((start+len) & PAGE_MASK))
	   return NULL;

	start = start & PAGE_MASK;
	flags = write ? (VM_WRITE | VM_MAYWRITE) : (VM_READ | VM_MAYREAD);

	vma = find_extend_vma(mm, start);

	if (!vma || (vma->vm_flags & VM_IO) || !(flags & vma->vm_flags))
	   return NULL;

	return follow_page(vma, start, write ? FOLL_WRITE : 0);
}

int mdb_user_read(void *to, void *from, size_t size)
{
	struct page *page;
	void *vaddr;

        if (!mdb_current_task)
           return size;

	page = mdb_get_user_page(mdb_current_task, from, size, 0);
	if (!page)
	   return size;

	vaddr = kmap_atomic(page, KM_TYPE_NR);
	memcpy(to, vaddr+ ((unsigned long)from & (PAGE_SIZE - 1)), size);
	kunmap_atomic(vaddr, KM_TYPE_NR);

	return 0;
}

int mdb_user_write(void *to, void *from, size_t size)
{
	struct page *page;
	void *vaddr;

        if (!mdb_current_task)
           return size;

	page = mdb_get_user_page(mdb_current_task, to, size, 1);
	if (!page)
		return size;

	vaddr = kmap_atomic(page, KM_TYPE_NR);
	memcpy(vaddr+ ((unsigned long)to & (PAGE_SIZE - 1)), from, size);
	kunmap_atomic(vaddr, KM_TYPE_NR);

	return 0;
}

int mdb_copy(void *to, void *from, size_t size)
{
    mm_segment_t oldfs = get_fs();
    int r;

     // map pages to user space if toggle is set and to/from < PAGE_OFFSET
    if (user_toggle)
    {
       if ((unsigned long)from < PAGE_OFFSET) 
          return mdb_user_read(to, from, size);

       if ((unsigned long)to < PAGE_OFFSET) 
          return mdb_user_write(to, from, size);
    }

    set_fs(KERNEL_DS);
    r = __copy_to_user_inatomic(to, from, size);
    set_fs(oldfs);
    return r;
}

int mdb_verify_rw(void *addr, size_t size)
{
    unsigned char data[size];
    return (mdb_copy(data, addr, size));
}

int mdb_getlword(uint64_t *word, unsigned long addr, size_t size)
{
	int err;

	__u8  w1;
	__u16 w2;
	__u32 w4;
	__u64 w8;

	*word = 0;	/* Default value if addr or size is invalid */
	switch (size) {
	case 1:
		if (!(err = mdb_copy(&w1, (void *)addr, size)))
			*word = w1;
		break;
	case 2:
		if (!(err = mdb_copy(&w2, (void *)addr, size)))
			*word = w2;
		break;
	case 4:
		if (!(err = mdb_copy(&w4, (void *)addr, size)))
			*word = w4;
		break;
	case 8:
		if (!(err = mdb_copy(&w8, (void *)addr, size)))
			*word = w8;
		break;
	default:
		err = -EFAULT;
	}
	return (err);
}

int mdb_putword(unsigned long addr, unsigned long word, size_t size)
{
	int err;
	__u8  w1;
	__u16 w2;
	__u32 w4;
	__u64 w8;

	switch (size) {
	case 1:
		w1 = word;
		err = mdb_copy((void *)addr, &w1, size);
		break;
	case 2:
		w2 = word;
		err = mdb_copy((void *)addr, &w2, size);
		break;
	case 4:
		w4 = word;
		err = mdb_copy((void *)addr, &w4, size);
		break;
	case 8:
	        w8 = word;
	        err = mdb_copy((void *)addr, &w8, size);
	        break;
	default:
		err = -EFAULT;
	}
	return (err);
}

int mdb_putqword(uint64_t *addr, uint64_t word, size_t size)
{
	int err;
	__u8  w1;
	__u16 w2;
	__u32 w4;
	__u64 w8;

	switch (size) {
	case 1:
		w1 = word;
		err = mdb_copy((void *)addr, &w1, size);
		break;
	case 2:
		w2 = word;
		err = mdb_copy((void *)addr, &w2, size);
		break;
	case 4:
		w4 = word;
		err = mdb_copy((void *)addr, &w4, size);
		break;
	case 8:
	        w8 = word;
	        err = mdb_copy((void *)addr, &w8, size);
	        break;
	default:
		err = -EFAULT;
	}
	return (err);
}

uint64_t mdb_getqword(uint64_t *addr, size_t size)
{
   uint64_t data = 0;
   register int ret;

   ret = mdb_getlword(&data, (unsigned long)addr, size);
   if (ret)
      return 0;

   return data;
}

unsigned long mdb_getword(unsigned long addr, size_t size)
{
   uint64_t data = 0;
   register int ret;

   ret = mdb_getlword((uint64_t *)&data, addr, size);
   if (ret)
      return 0;

   return (unsigned long)data;
}

uint64_t mdb_segment_getqword(unsigned long sv, uint64_t *addr, size_t size)
{
   uint64_t data = 0;
   register int ret;

   ret = mdb_getlword(&data, (unsigned long)addr, size);
   if (ret)
      return 0;

   return data;
}

unsigned long mdb_segment_getword(unsigned long sv, unsigned long addr, 
                                  size_t size)
{
   uint64_t data = 0;
   register int ret;

   ret = mdb_getlword((uint64_t *)&data, addr, size);
   if (ret)
      return 0;

   return (unsigned long)data;
}

int DisplayClosestSymbol(unsigned long address)
{
    char *modname;
    const char *name;
    unsigned long offset = 0, size;
    char namebuf[KSYM_NAME_LEN+1];

    name = kallsyms_lookup(address, &size, &offset, &modname, namebuf);
    if (!name)
       return -1;

    if (modname)
    {
       if (offset)
          DBGPrint("%s|%s+0x%X\n", modname, name, offset);
       else
          DBGPrint("%s|%s\n", modname, name);
    }
    else
    {
       if (offset)
          DBGPrint("%s+0x%X\n", name, offset);
       else
          DBGPrint("%s\n", name);
    }
    return 0;
}

void DumpOSSymbolTableMatch(unsigned char *symbol)
{
    mdb_kallsyms(symbol, DBGPrint);
    return;
}

unsigned long GetValueFromSymbol(unsigned char *symbol)
{
   return ((unsigned long)kallsyms_lookup_name(symbol));
}


unsigned char *GetModuleInfoFromSymbolValue(unsigned long value, unsigned char *buf, unsigned long len)
{
    char *modname;
    const char *name;
    unsigned long offset, size;
    char namebuf[KSYM_NAME_LEN+1];

    name = kallsyms_lookup(value, &size, &offset, &modname, namebuf);
    if (modname && buf)
    {
       strncpy(buf, modname, len);
       return (unsigned char *)buf;
    }
    return NULL;
}

unsigned char *GetSymbolFromValue(unsigned long value, unsigned char *buf, unsigned long len)
{
    char *modname;
    const char *name;
    unsigned long offset, size;
    char namebuf[KSYM_NAME_LEN+1];

    name = kallsyms_lookup(value, &size, &offset, &modname, namebuf);
    if (!name)
       return NULL;

    if (!offset && buf)
    {
       strncpy(buf, namebuf, len);
       return (unsigned char *)buf;
    }

    return NULL;
}

unsigned char *GetSymbolFromValueWithOffset(unsigned long value, unsigned long *sym_offset,
                                   unsigned char *buf, unsigned long len)
{
    char *modname;
    const char *name;
    unsigned long offset, size;
    char namebuf[KSYM_NAME_LEN+1];

    name = kallsyms_lookup(value, &size, &offset, &modname, namebuf);
    if (!name || !buf)
       return NULL;

    if (sym_offset)
      *sym_offset = offset;

    strncpy(buf, namebuf, len);
    return (unsigned char *)buf;
}

unsigned char *GetSymbolFromValueOffsetModule(unsigned long value, unsigned long *sym_offset,
                                     unsigned char **module, unsigned char *buf, unsigned long len)
{
    char *modname;
    const char *name;
    unsigned long offset, size;
    char namebuf[KSYM_NAME_LEN+1];

    name = kallsyms_lookup(value, &size, &offset, &modname, namebuf);
    if (!name || !buf)
       return NULL;

    if (sym_offset)
      *sym_offset = offset;

    if (module)
       *module = modname;

    strncpy(buf, namebuf, len);
    return (unsigned char *)buf;
}


unsigned long get_processor_id(void)
{
#if defined(CONFIG_SMP)
   return raw_smp_processor_id();
#else
   return 0;
#endif
}

unsigned long get_physical_processor(void)
{
#if defined(CONFIG_SMP)
   return raw_smp_processor_id();
#else
   return 0;
#endif
}

unsigned long fpu_present(void)
{
   if (boot_cpu_has(X86_FEATURE_FPU))
       return 1;
    return 0;
}

extern unsigned long cpu_mttr_on(void)
{
   if (boot_cpu_has(X86_FEATURE_MTRR))
       return 1;
    return 0;
}

unsigned char *UpcaseString(unsigned char *s)
{
   register int i;

   for (i = 0; i < strlen(s); i++)
      s[i] = toupper(s[i]);
   return s;

}
void ClearScreen(void)
{
    mdb_printf("%c%c", 0x1B, 'c');
    return;
}

unsigned long ReadDS(void)
{
    unsigned short contents = 0;

    __asm__ ("mov %%ds,%0\n\t":"=r"(contents));
    return contents;
}

unsigned long ReadES(void)
{
    unsigned short contents = 0;

    __asm__ ("mov %%es,%0\n\t":"=r"(contents));
    return contents;
}

unsigned long ReadFS(void)
{
    unsigned short contents = 0;

    __asm__ ("mov %%fs,%0\n\t":"=r"(contents));
    return contents;
}

unsigned long ReadGS(void)
{
    unsigned short contents = 0;

    __asm__ ("mov %%gs,%0\n\t":"=r"(contents));
    return contents;
}

#ifdef CONFIG_X86_64
unsigned long ReadDR(unsigned long regnum)
{
	unsigned long contents = 0;

	switch(regnum)
        {
	   case 0:
		__asm__ ("movq %%db0,%0\n\t":"=r"(contents));
		break;
	   case 1:
		__asm__ ("movq %%db1,%0\n\t":"=r"(contents));
		break;
	   case 2:
		__asm__ ("movq %%db2,%0\n\t":"=r"(contents));
		break;
	   case 3:
		__asm__ ("movq %%db3,%0\n\t":"=r"(contents));
		break;
	   case 4:
	   case 5:
		break;
	   case 6:
		__asm__ ("movq %%db6,%0\n\t":"=r"(contents));
		break;
	   case 7:
		__asm__ ("movq %%db7,%0\n\t":"=r"(contents));
		break;
	   default:
		break;
	}

	return contents;
}

void WriteDR(int regnum, unsigned long contents)
{
	switch(regnum)
        {
	   case 0:
		__asm__ ("movq %0,%%db0\n\t"::"r"(contents));
		break;
	   case 1:
		__asm__ ("movq %0,%%db1\n\t"::"r"(contents));
		break;
	   case 2:
		__asm__ ("movq %0,%%db2\n\t"::"r"(contents));
		break;
	   case 3:
		__asm__ ("movq %0,%%db3\n\t"::"r"(contents));
		break;
	   case 4:
	   case 5:
		break;
	   case 6:
		__asm__ ("movq %0,%%db6\n\t"::"r"(contents));
		break;
	   case 7:
		__asm__ ("movq %0,%%db7\n\t"::"r"(contents));
		break;
	   default:
		break;
	}
}

unsigned long ReadCR(int regnum)
{
	unsigned long contents = 0;

	switch(regnum)
        {
	   case 0:
		__asm__ ("movq %%cr0,%0\n\t":"=r"(contents));
		break;
	   case 1:
		break;
	   case 2:
		__asm__ ("movq %%cr2,%0\n\t":"=r"(contents));
		break;
	   case 3:
		__asm__ ("movq %%cr3,%0\n\t":"=r"(contents));
		break;
	   case 4:
		__asm__ ("movq %%cr4,%0\n\t":"=r"(contents));
		break;
	   default:
		break;
	}
	return contents;
}

void WriteCR(int regnum, unsigned long contents)
{
	switch(regnum)
        {
	   case 0:
		__asm__ ("movq %0,%%cr0\n\t"::"r"(contents));
		break;
	   case 1:
		break;
	   case 2:
		__asm__ ("movq %0,%%cr2\n\t"::"r"(contents));
		break;
	   case 3:
		__asm__ ("movq %0,%%cr3\n\t"::"r"(contents));
		break;
	   case 4:
		__asm__ ("movq %0,%%cr4\n\t"::"r"(contents));
		break;
	   default:
		break;
	}
	return;
}

#else
unsigned long ReadDR(unsigned long regnum)
{
	unsigned long contents = 0;

	switch(regnum)
        {
	   case 0:
		__asm__ ("movl %%db0,%0\n\t":"=r"(contents));
		break;
	   case 1:
		__asm__ ("movl %%db1,%0\n\t":"=r"(contents));
		break;
	   case 2:
		__asm__ ("movl %%db2,%0\n\t":"=r"(contents));
		break;
	   case 3:
		__asm__ ("movl %%db3,%0\n\t":"=r"(contents));
		break;
	   case 4:
	   case 5:
		break;
	   case 6:
		__asm__ ("movl %%db6,%0\n\t":"=r"(contents));
		break;
	   case 7:
		__asm__ ("movl %%db7,%0\n\t":"=r"(contents));
		break;
	   default:
		break;
	}

	return contents;
}

void WriteDR(int regnum, unsigned long contents)
{
	switch(regnum)
        {
	   case 0:
		__asm__ ("movl %0,%%db0\n\t"::"r"(contents));
		break;
	   case 1:
		__asm__ ("movl %0,%%db1\n\t"::"r"(contents));
		break;
	   case 2:
		__asm__ ("movl %0,%%db2\n\t"::"r"(contents));
		break;
	   case 3:
		__asm__ ("movl %0,%%db3\n\t"::"r"(contents));
		break;
	   case 4:
	   case 5:
		break;
	   case 6:
		__asm__ ("movl %0,%%db6\n\t"::"r"(contents));
		break;
	   case 7:
		__asm__ ("movl %0,%%db7\n\t"::"r"(contents));
		break;
	   default:
		break;
	}
}

unsigned long ReadCR(int regnum)
{
	unsigned long contents = 0;

	switch(regnum)
        {
	   case 0:
		__asm__ ("movl %%cr0,%0\n\t":"=r"(contents));
		break;
	   case 1:
		break;
	   case 2:
		__asm__ ("movl %%cr2,%0\n\t":"=r"(contents));
		break;
	   case 3:
		__asm__ ("movl %%cr3,%0\n\t":"=r"(contents));
		break;
	   case 4:
		__asm__ ("movl %%cr4,%0\n\t":"=r"(contents));
		break;
	   default:
		break;
	}
	return contents;
}

void WriteCR(int regnum, unsigned long contents)
{
	switch(regnum)
        {
	   case 0:
		__asm__ ("movl %0,%%cr0\n\t"::"r"(contents));
		break;
	   case 1:
		break;
	   case 2:
		__asm__ ("movl %0,%%cr2\n\t"::"r"(contents));
		break;
	   case 3:
		__asm__ ("movl %0,%%cr3\n\t"::"r"(contents));
		break;
	   case 4:
		__asm__ ("movl %0,%%cr4\n\t"::"r"(contents));
		break;
	   default:
		break;
	}
	return;
}
#endif

unsigned long ReadTR(void)
{
   unsigned short tr;

   __asm__ __volatile__("str %0":"=a"(tr));

   return (unsigned long) tr;
}

unsigned long ReadLDTR(void)
{
   unsigned short ldt;

   __asm__ __volatile__("sldt %0":"=a"(ldt));

   return (unsigned long) ldt;
}

void ReadGDTR(unsigned long *v)
{
   __asm__ __volatile__("sgdt %0":"=m"(*v));
}

void ReadIDTR(unsigned long *v)
{
    __asm__ __volatile__("sidt %0":"=m"(*v));
}

void save_npx(NUMERIC_FRAME *v)
{
    __asm__ __volatile__("fsave %0":"=m"(*v));
}

void load_npx(NUMERIC_FRAME *v)
{
    __asm__ __volatile__("frstor %0":"=m"(*v));
}

unsigned long ReadDR6(void)  {  return (ReadDR(6)); }

unsigned long ReadDR0(void)  {  return (ReadDR(0)); }
unsigned long ReadDR1(void)  {  return (ReadDR(1)); }
unsigned long ReadDR2(void)  {  return (ReadDR(2)); }
unsigned long ReadDR3(void)  {  return (ReadDR(3)); }
unsigned long ReadDR7(void)  {  return (ReadDR(7)); }

void WriteDR0(unsigned long v) { WriteDR(0, v); }
void WriteDR1(unsigned long v) { WriteDR(1, v); }
void WriteDR2(unsigned long v) { WriteDR(2, v); }
void WriteDR3(unsigned long v) { WriteDR(3, v); }
void WriteDR6(unsigned long v) { WriteDR(6, v); }
void WriteDR7(unsigned long v) { WriteDR(7, v); }

unsigned long ReadCR0(void) {  return (ReadCR(0)); }
unsigned long ReadCR2(void) {  return (ReadCR(2)); }
unsigned long ReadCR3(void) {  return (ReadCR(3)); }
unsigned long ReadCR4(void) {  return (ReadCR(4)); }

void WriteCR0(unsigned long v) { WriteCR(0, v); }
void WriteCR2(unsigned long v) { WriteCR(2, v); }
void WriteCR3(unsigned long v) { WriteCR(3, v); }
void WriteCR4(unsigned long v) { WriteCR(4, v); }

void ReadMSR(unsigned long r, unsigned long *v1, unsigned long *v2)
{
    unsigned long vv1, vv2;

    rdmsr(r, vv1, vv2);

    if (v1)
       *v1 = vv1;
    if (v2)
       *v2 = vv2;
}

void WriteMSR(unsigned long r, unsigned long *v1, unsigned long *v2)
{
    unsigned long vv1 = 0, vv2 = 0;

    if (v1)
       vv1 = *v1;
    if (v2)
       vv2 = *v2;

    wrmsr(r, vv1, vv2);
}

