
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
#include <asm/segment.h>
#include <asm/atomic.h>
#include <asm/msr.h>
#include <linux/io.h>
#include <linux/clocksource.h>

#if defined(CONFIG_SMP)
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

#if defined(CONFIG_SMP) && !defined(CONFIG_X86_ELAN)

extern unsigned int io_apic_read(unsigned int apic, unsigned int reg);
extern int nr_ioapics;

unsigned long apic_directed_nmi(unsigned long cpu)
{
    apic->send_IPI_mask(cpumask_of(cpu), 
                        APIC_DM_NMI | APIC_INT_LEVELTRIG | APIC_INT_ASSERT);
    apic->send_IPI_mask(cpumask_of(cpu), 
                        APIC_DM_NMI | APIC_INT_LEVELTRIG);
    return 0;
}

void dump_ioapic(unsigned long num)
{
     unsigned long i, val;

     if (num < nr_ioapics)
     {
        DBGPrint("io_apic registers\n");
        for (i = 0; i <= 0x2F; i++)
        {
	   if ((i & 3) == 0)
	      DBGPrint("%08X: ", i);

	   val = io_apic_read(num, i * 4);
	   DBGPrint("%08X ", val);

	   if ((i & 3) == 3)
	      DBGPrint("\n");
        }
     }
     return;
}

void dump_local_apic(void)
{
    unsigned long i, val;

    DBGPrint("local apic registers\n");
    for (i = 0; i <= 0x3F; i++)
    {
       if ((i & 3) == 0)
	  DBGPrint("%08X: ", i);

       val = apic_read(i * 4);
       DBGPrint("%08X ", val);

       if ((i & 3) == 3)
	  DBGPrint("\n");
    }

}

void dump_remote_apic(unsigned long cpu)
{
    register unsigned long i, timeout, apicid;
    register unsigned long val;

    DBGPrint("remote apic registers processor(%d)\n", cpu);
    for (i = 0; i <= 0x3F; i++)
    {
       if ((i & 3) == 0)
	  DBGPrint("%08X: ", i);

       apicid = apic->cpu_present_to_apicid(cpu);
       if (apicid == BAD_APICID)
       {
          DBGPrint("BADAPICX ");
          continue;
       }

       timeout = 0;
       while (apic_read(APIC_ICR) & APIC_ICR_BUSY)
       {
          udelay(100);
          if (timeout++ >= 1000)
             break;
          cpu_relax();
          touch_nmi_watchdog();
       }

       if (timeout >= 1000)
       {
          DBGPrint("???????? ");
          continue;
       }

       apic_write(APIC_ICR2, SET_APIC_DEST_FIELD(apicid));
       apic_write(APIC_ICR, i | APIC_DEST_LOGICAL | APIC_DM_REMRD);

       timeout = 0;
       while ((apic_read(APIC_ICR) & APIC_ICR_RR_MASK) == APIC_ICR_RR_INPROG)
       {
          udelay(100);
          if (timeout++ >= 1000)
             break;

          cpu_relax();
          touch_nmi_watchdog();
       }

       if (timeout >= 1000)
       {
          DBGPrint("???????? ");
          continue;
       }

       if ((apic_read(APIC_ICR) & APIC_ICR_RR_MASK) == APIC_ICR_RR_VALID)
       {
          val = apic_read(APIC_RRR);
          DBGPrint("%08X ", val);
       }
       else
       {
          DBGPrint("???????? ");
       }

       if ((i & 3) == 3)
	  DBGPrint("\n");
    }

}

unsigned long displayAPICHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("apic                     - display local apic regs\n");
    DBGPrint("apic [p#]                - display remote apic regs\n");
    return 1;
}

/* APIC */

unsigned long displayAPICInfo(unsigned char *cmd,
		     StackFrame *stackFrame, unsigned long Exception,
		     DEBUGGER_PARSER *parser)
{
     register unsigned long value;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid && ((value >= MAX_PROCESSORS) || !cpu_online(value)))
     {
        DBGPrint("processor not found\n");
        return 1;
     }
     if (valid && (value != get_processor_id()))
        dump_remote_apic(value);
     else
        dump_local_apic();
     return 1;
}

unsigned long displayIOAPICHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("ioapic [#]               - display specified ioapic [#] regs\n");
    return 1;
}

/* IOAPIC */

unsigned long displayIOAPICInfo(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     register unsigned long value;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     value = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid && !(value < nr_ioapics))
     {
        DBGPrint("ioapic not found\n");
        return 1;
     }
     if (valid)
        dump_ioapic(value);
     else
        dump_ioapic(0);

     return 1;

}

#endif /* CONFIG_SMP */
