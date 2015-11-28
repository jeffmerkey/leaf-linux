
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
#include <linux/kdebug.h>
#include <linux/notifier.h>
#include <linux/sysrq.h>
#include <linux/input.h>

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

extern void MDBInitializeDebugger(void);
extern void MDBClearDebuggerState(void);

unsigned long TotalSystemMemory;
unsigned long HistoryPointer;
unsigned char HistoryBuffer[16][256];
                              /* remember non-repeating commands */
unsigned char delim_table[256];
unsigned char workBuffer[256];
unsigned char verbBuffer[100];
void set_delimiter(unsigned char c) {  delim_table[c & 0xFF] = 1;  }
StackFrame CurrentStackFrame[MAX_PROCESSORS];
atomic_t inmdb = { 0 };
unsigned char *mdb_oops;
unsigned char *last_mdb_oops;

void SaveLastCommandInfo(unsigned long processor)
{
    register int i;

    repeatCommand = 0;
    lastCommand = toupper(debugCommand[0]);
    lastDisplayLength = displayLength;
    atomic_set(&focusActive, 0);
    atomic_set(&traceProcessors[processor], 0);

    for (i = 0; (i < 80) && (debugCommand[i]); i++)
    {
       if ((debugCommand[i] == '\n') || (debugCommand[i] == '\r'))
          lastDebugCommand[i] = '\0';
       else
          lastDebugCommand[i] = debugCommand[i];
    }
    lastDebugCommand[i] = '\0';

    return;
}

int mdb(int reason, int error, void *frame)
{
    register unsigned long retCode = 0, processor = get_processor_id();
    extern void ReadStackFrame(void *, StackFrame *, unsigned long);
    extern void WriteStackFrame(void *, StackFrame *, unsigned long);
    unsigned long state;
#ifdef CONFIG_MDB_CONSOLE_REDIRECTION
    extern int kmsg_redirect;
    int kmsg_redirect_save;
#endif

    retCode = AlternateDebuggerRoutine(reason, error, frame);
    if (retCode)
       return retCode;

    preempt_disable();
#ifdef CONFIG_MDB_CONSOLE_REDIRECTION
    kmsg_redirect_save = kmsg_redirect;
    kmsg_redirect = 0;
#endif

    last_mdb_oops = NULL;
    if (mdb_oops)
    {
       last_mdb_oops = mdb_oops;
       mdb_oops = NULL;
    }
    atomic_inc(&inmdb);
    state = save_flags();
    memset(&CurrentStackFrame[processor], 0, sizeof(StackFrame));
    ReadStackFrame(frame, &CurrentStackFrame[processor], processor);
    CurrentStackFrame[processor].tReserved[2] = (unsigned long)frame;
    retCode = debugger_entry(reason, &CurrentStackFrame[processor],
                             processor);
    WriteStackFrame(frame, &CurrentStackFrame[processor], processor);

    restore_flags(state);
    atomic_dec(&inmdb);

#ifdef CONFIG_MDB_CONSOLE_REDIRECTION
    kmsg_redirect = kmsg_redirect_save;
#endif
    preempt_enable();

    return retCode;
}

static inline void out_chars(int c, unsigned char ch)
{
    register int i;

    for (i = 0; i < c; i++)
       DBGPrint("%c", ch);
    return;
}

static inline void out_copy_chars_limit(int c, unsigned char ch,
                                        unsigned char *s, int limit)
{
    register int i;

    for (i = 0; (i < c) && (i < limit); i++)
       s[i] = ch;
    s[i] = '\0';
    DBGPrint("%s", s);
    return;
}

static inline void out_buffer_chars_limit(unsigned char *s, int limit)
{
    register int i;

    for (i = 0; (i < strlen(s)) && (i < limit); i++)
       DBGPrint("%c", s[i]);
    return;
}

static inline void out_buffer_chars_limit_index(unsigned char *s, int limit,
                                                int index)
{
    register int i;

    for (i = 0; (i < limit) && (s[i]); i++)
       DBGPrint("%c", s[index + i]);
    return;
}

static inline void out_string(unsigned char *s)
{
    DBGPrint("%s", s);
    return;
}

static inline void out_char(unsigned char ch)
{
    DBGPrint("%c", ch);
    return;
}

unsigned long ScreenInputFromKeyboard(unsigned char *buf,
                                      unsigned long buf_index,
                                      unsigned long max_index)
{
    register unsigned long key;
    register unsigned char *p;
    register int i, r, temp;
    register unsigned long orig_index, HistoryIndex;
    extern unsigned long IsAccelerator(unsigned long);

    if (buf_index > max_index)
       return 0;

    if (!max_index)
       return 0;

    orig_index = buf_index;

    p = (unsigned char *)((unsigned long)buf + (unsigned long)buf_index);
    for (i = 0; i < (max_index - buf_index); i++)
       *p++ = '\0';

    HistoryIndex = HistoryPointer;
    while (1)
    {
       key = mdb_getkey();

       if ((IsAccelerator(key)) && (key != 13))
	  return key;

       switch (key)
       {
	  case 8: /* backspace */
	     if (buf_index)
	     {
                register int delta;

		buf_index--;
		out_string("\b \b");

                delta = strlen(buf) - buf_index;
                out_chars(delta, ' ');
                out_chars(delta, '\b');

	        p = (unsigned char *) &buf[buf_index];
	        temp = buf_index;
	        p++;
	        while ((*p) && (temp < max_index))
		   buf[temp++] = *p++;
	        buf[temp] = '\0';

                delta = strlen(buf) - buf_index;
                out_buffer_chars_limit_index(buf, delta, buf_index);
                out_chars(delta, '\b');
	     }
	     break;

	  case K_P7: /* home */
             {
                unsigned char *s = &workBuffer[0];

                out_copy_chars_limit(buf_index, '\b', s, 255);
	        buf_index = orig_index;
             }
	     break;

	  case K_P1: /* end */
             {
                unsigned char *s = &workBuffer[0];

                out_copy_chars_limit(buf_index, '\b', s, 255);
                out_buffer_chars_limit(buf, 255);
	        buf_index = strlen(buf);
             }
	     break;

	  case K_P4: /* left arrow */
	     if (buf_index)
	     {
		buf_index--;
		out_string("\b");
	     }
	     break;

	  case K_P6: /* right arrow */
	     if (buf_index < strlen(buf))
	     {
                out_char(buf[buf_index]);
		buf_index++;
	     }
	     break;

	  case K_PDOT:
             {
                register int delta;

                delta = strlen(buf) - buf_index;

                out_chars(delta, ' ');
                out_chars(delta, '\b');

	        p = (unsigned char *) &buf[buf_index];
	        temp = buf_index;
	        p++;
	        while ((*p) && (temp < max_index))
		   buf[temp++] = *p++;
	        buf[temp] = '\0';

                delta = strlen(buf) - buf_index;
                out_buffer_chars_limit_index(buf, delta, buf_index);
                out_chars(delta, '\b');
             }
	     break;

	  case 13:  /* enter */
	     if (strncmp(HistoryBuffer[(HistoryPointer - 1) & 0xF], buf,
                         strlen(buf)) || (strlen(buf) !=
                         strlen(HistoryBuffer[(HistoryPointer - 1) & 0xF])))
	     {
		for (r = 0; r < max_index; r++)
		{
		   if (buf[0])
		      HistoryBuffer[HistoryPointer & 0xF][r] = buf[r];
		}
		if (buf[0])
		   HistoryPointer++;
	     }
	     return 13;

	  case K_P8: /* up arrow */
	     if (HistoryBuffer[(HistoryIndex - 1) & 0xF][0])
	     {
                unsigned char *s = &workBuffer[0];

                out_copy_chars_limit(buf_index, '\b', s, 255);
                out_copy_chars_limit(buf_index, ' ', s, 255);
                out_copy_chars_limit(buf_index, '\b', s, 255);

		HistoryIndex--;

		for (r = 0; r < max_index; r++)
		   buf[r] = HistoryBuffer[HistoryIndex & 0xF][r];
		buf_index = strlen(buf);

                out_string(buf);
	     }
	     break;

	  case K_P2: /* down arrow */
	     if (HistoryBuffer[HistoryIndex & 0xF][0])
	     {
                unsigned char *s = &workBuffer[0];

                out_copy_chars_limit(buf_index, '\b', s, 255);
                out_copy_chars_limit(buf_index, ' ', s, 255);
                out_copy_chars_limit(buf_index, '\b', s, 255);

		HistoryIndex++;

		for (r = 0; r < max_index; r++)
		   buf[r] = HistoryBuffer[HistoryIndex & 0xF][r];
		buf_index = strlen(buf);

                out_string(buf);
	     }
	     break;

	  default:
	     if ((key > 0x7E) || (key < ' '))  /* if above or below text */
		break;
	     else
	     {
	        if (strlen(buf) < max_index)
		{
                   register int delta;

		   for (i=max_index; i > buf_index; i--)
		      buf[i] = buf[i-1];
		   buf[buf_index] = (unsigned char)key;
		   if (buf_index < max_index)
		      buf_index++;

                   delta = strlen(buf) - buf_index;
                   out_buffer_chars_limit_index(buf, delta, buf_index);
                   out_chars(delta, '\b');
		}
             }
	     break;
       }
    }
}

unsigned long debugger_command_entry(unsigned long processor,
                                     unsigned long Exception,
			             StackFrame *stackFrame)
{
    register unsigned char *verb, *pp, *vp;
    register unsigned long count, retCode, key;
    extern unsigned long reason_toggle;
    extern void displayRegisters(StackFrame *, unsigned long);

    if (Exception > 22)
       Exception = 20;

    lastUnasmAddress = (unsigned long) GetIP(stackFrame);
    lastLinkAddress = lastDumpAddress =
                      (unsigned char *) GetStackAddress(stackFrame);
    lastDisplayLength = displayLength = 20;
    lastCommandEntry = lastCommand;
    nextline = 0;
    pause_mode = 0;

    if (!ssbmode)
    {
       if (reason_toggle && !ConsoleDisplayBreakReason(stackFrame,
           Exception, processor, lastCommand))
          return 0;
       displayRegisters(stackFrame, processor);
    }
    nextUnasmAddress = disassemble(stackFrame, lastUnasmAddress, 1, 1);
    ClearTempBreakpoints();
    if (SSBUpdate(stackFrame, processor) == -1)
       return 0;

    while (1)
    {
       pause_mode = 1;
       nextline = 0;
       DBGPrint("(%i)> ", (int)processor);

       SaveLastCommandInfo(processor);
       key = ScreenInputFromKeyboard((unsigned char *)&debugCommand[0], 0, 80);
       if (key)
       {
          extern unsigned long AccelRoutine(unsigned long key, void *p);

          retCode = AccelRoutine(key, stackFrame);
          switch (retCode)
          {
             case 0:
                break;

             case -1:
	        return retCode;

             default:
                DBGPrint("\n");
                continue;
          }
       }

       if (*debugCommand)
       {
          count = 0;
          pp = (unsigned char *)debugCommand;
          vp = verb = &verbBuffer[0];
          while (*pp && *pp == ' ' && count++ < 80)
	     pp++;

          while (*pp && *pp != ' ' && *pp != '=' && count++ < 80)
	     *vp++ = *pp++;
          *vp = '\0';

          while (*pp && *pp == ' ' && count++ < 80)
	     pp++;

          retCode = DebuggerParserRoutine(verb, (unsigned char *)debugCommand,
                                          stackFrame, Exception);
          switch (retCode)
          {
             case -1:
	       return retCode;
          }
       }
    }
}

#if MDB_DEBUG_DEBUGGER
unsigned char *kdebug_state[]=
{
    "",
    "DIE_OOPS",
    "DIE_INT3",
    "DIE_DEBUG",
    "DIE_PANIC",
    "DIE_NMI",
    "DIE_DIE",
    "DIE_NMIWATCHDOG",
    "DIE_KERNELDEBUG",
    "DIE_TRAP",
    "DIE_GPF",
    "DIE_CALL",
    "DIE_NMI_IPI",
    "DIE_PAGE_FAULT",
    "DIE_NMIUNKNOWN",
};
int kdebug_state_size = sizeof(kdebug_state) / sizeof(unsigned char *);
#endif

static int mdb_notify(struct notifier_block *nb, unsigned long reason,
                      void *data)
{
    register struct die_args *args = (struct die_args *)data;
    register int err = 0;
#if MDB_DEBUG_DEBUGGER
    register unsigned long p = get_processor_id();

    oops_in_progress++;
    DBGPrint("%i: notify reason:%lu %s\n",
             (int)p, reason, kdebug_state[reason % kdebug_state_size]);
#else
    oops_in_progress++;
#endif
    if (args)
    {
       switch (reason)
       {
          case DIE_DIE:
          case DIE_PANIC:
          case DIE_OOPS:
             mdb_oops = (unsigned char *)args->str;
             if (args->regs)
                err = mdb(SOFTWARE_EXCEPTION, args->err, args->regs);
             else
             {
                oops_in_progress--;
                mdb_breakpoint();
                return NOTIFY_STOP;
             }
             break;

	  case DIE_INT3:
             err = mdb(BREAKPOINT_EXCEPTION, args->err, args->regs);
             break;

          case DIE_DEBUG:
             err = mdb(DEBUGGER_EXCEPTION, args->err, args->regs);
             break;

          case DIE_NMI:
          case DIE_NMIWATCHDOG:
          case DIE_NMI_IPI:
             err = mdb(NMI_EXCEPTION, args->err, args->regs);
             break;

          case DIE_KERNELDEBUG:
             err = mdb(KEYBOARD_ENTRY, args->err, args->regs);
             break;

          case DIE_GPF:
             err = mdb(GENERAL_PROTECTION, args->err, args->regs);
             break;

          case DIE_PAGE_FAULT:
             err = mdb(PAGE_FAULT_EXCEPTION, args->err, args->regs);
             break;

          default:
             break;
       }
    }
    oops_in_progress--;
    return (err ? NOTIFY_STOP : 0);
}

static struct notifier_block mdb_notifier =
{
    .notifier_call = mdb_notify,
    .priority = 0x7FFFFFFF,
};

#ifdef CONFIG_MAGIC_SYSRQ
static void sysrq_mdb(int key, struct tty_struct *tty)
{
    mdb_breakpoint();
    return;
}

static struct sysrq_key_op sysrq_op =
{
    .handler     = sysrq_mdb,
    .help_msg    = "mdb(J)",
    .action_msg  = "MDB",
};
#endif

static int __init mdb_init_module(void)
{
    register int i;
    register int ret = 0;

    /* return if debugger already initialized */
    if (debuggerInitialized)
       return 0;

    MDBInitializeDebugger();

    ret = register_die_notifier(&mdb_notifier);
    if (ret)
    {
       MDBClearDebuggerState();
       return ret;
    }

#ifdef CONFIG_MAGIC_SYSRQ
    register_sysrq_key('j', &sysrq_op);
#endif

    /* initialize delimiter lookup table */
    for (i = 0; i < 256; i++)
        delim_table[i] = '\0';

    set_delimiter('\0');
    set_delimiter('\n');
    set_delimiter('\r');
    set_delimiter('[');
    set_delimiter(']');
    set_delimiter('<');
    set_delimiter('>');
    set_delimiter('(');
    set_delimiter(')');
    set_delimiter('|');
    set_delimiter('&');
    set_delimiter('=');
    set_delimiter('*');
    set_delimiter('+');
    set_delimiter('-');
    set_delimiter('/');
    set_delimiter('%');
    set_delimiter('~');
    set_delimiter('^');
    set_delimiter('!');
    set_delimiter(' ');

    return 0;
}

static void __exit mdb_exit_module(void)
{
#ifdef CONFIG_MAGIC_SYSRQ
    unregister_sysrq_key('j', &sysrq_op);
#endif
    unregister_die_notifier(&mdb_notifier);
    MDBClearDebuggerState();
    return;
}

module_init(mdb_init_module);
module_exit(mdb_exit_module);

MODULE_DESCRIPTION("Merkey's Kernel Debugger");
MODULE_LICENSE("GPL");
