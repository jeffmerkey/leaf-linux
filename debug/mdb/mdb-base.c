
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

unsigned long debug_deref;
unsigned long full_deref_toggle;
unsigned long control_toggle;
unsigned long numeric_toggle;
unsigned long general_toggle = 1;
unsigned long line_info_toggle = 1;
unsigned long segment_toggle = 1;
unsigned long reason_toggle = 1;

unsigned long enterKeyACC(unsigned long key, void *stackFrame,
	          ACCELERATOR *accel)
{
    unsigned char *verbBuffer = &workbuf[0][0];
    register unsigned char *verb, *pp, *vp;
    register unsigned long count;

    if (!debugCommand[0])
    {
       count = 0;
       pp = (unsigned char *)lastDebugCommand;
       vp = verb = &verbBuffer[0];
       while (*pp && *pp == ' ' && count++ < 80)
	  pp++;

       while (*pp && *pp != ' ' && count++ < 80)
	  *vp++ = *pp++;
       *vp = '\0';

       while (*pp && *pp == ' ' && count++ < 80)
	  pp++;

       UpcaseString(verb);
       if (!strcmp(verb, "P") || (lastCommand == K_F8))
	  strcpy((char *)debugCommand, "P");
       else
       if (!strcmp(verb, "T") || (lastCommand == K_F7))
	  strcpy((char *)debugCommand, "T");
       else
       if (!strcmp(verb, "W")   || !strcmp(verb, "D")   ||
	   !strcmp(verb, "DB")  || !strcmp(verb, "DW")  ||
	   !strcmp(verb, "DD")  || !strcmp(verb, "DDS") ||
	   !strcmp(verb, "DS")  || !strcmp(verb, "DL")  ||
	   !strcmp(verb, "U")   || !strcmp(verb, "UU")  ||
	   !strcmp(verb, "S")   || !strcmp(verb, "SS")  ||
	   !strcmp(verb, "SSB") || !strcmp(verb, "ID")) {
	  strcpy((char *)debugCommand, verb);
	  repeatCommand = 1;
       }
    }
    return 0;

}


unsigned long displayDebuggerHelpHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("displays general help for all commands, or help for a specific command\n");
    DBGPrint("HELP         <enter>  - list all commands\n");
    DBGPrint("HELP command <enter>  - help for a specific command\n");

    return 1;
}

unsigned long displayDebuggerHelp(unsigned char *commandLine,
			 StackFrame *stackFrame, unsigned long Exception,
			 DEBUGGER_PARSER *parser)
{

    register unsigned long count;
    unsigned char *verbBuffer = &workbuf[0][0];
    register unsigned char *verb, *pp, *vp;

    commandLine = &commandLine[parser->debugCommandNameLength];
    while (*commandLine && *commandLine == ' ')
       commandLine++;

    count = 0;
    pp = commandLine;
    vp = verb = &verbBuffer[0];
    while (*pp && *pp == ' ' && count++ < 80)
       pp++;

    while (*pp && *pp != ' ' && count++ < 80)
       *vp++ = *pp++;
    *vp = '\0';

    while (*pp && *pp == ' ' && count++ < 80)
       pp++;

    DebuggerParserHelpRoutine(verb, commandLine);
    return 1;

}

/* TIMER */

struct timer_list debug_timer;

void debug_timer_callback(void)
{
    debug_timer.expires = jiffies + (HZ * debug_timer.data);
    debug_timer.function = (void (*)(unsigned long))debug_timer_callback;
    add_timer(&debug_timer);
    mdb_breakpoint();
    return;
}

unsigned long timedBreakpointHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("addtimer <seconds>      - add int3 timer (lockup detection)\n");
    DBGPrint("deltimer                - del int3 timer\n");
    return 1;
}

unsigned long timerBreakpoint(unsigned char *cmd, StackFrame *stackFrame,
                            unsigned long Exception, DEBUGGER_PARSER *parser)
{
    register int seconds;
    unsigned long valid = 0;

    cmd = &cmd[parser->debugCommandNameLength];
    while (*cmd && *cmd == ' ')
       cmd++;

    if (debug_timer.data)
    {
       DBGPrint("debug timer is already active.  seconds = %i\n",
                (int)debug_timer.data);
       return 1;
    }

    seconds = EvaluateNumericExpression(stackFrame, &cmd, &valid);
    if (valid)
    {
       init_timer(&debug_timer);
       debug_timer.data = seconds;
       debug_timer.expires = jiffies + (HZ * seconds);
       debug_timer.function = (void (*)(unsigned long))debug_timer_callback;
       add_timer(&debug_timer);

       DBGPrint("debug timer created.  seconds = %i\n", seconds);
    }
    return 1;
}

unsigned long timerBreakpointClear(unsigned char *cmd, StackFrame *stackFrame,
                                   unsigned long Exception,
                                   DEBUGGER_PARSER *parser)
{
    del_timer(&debug_timer);
    debug_timer.data = 0;
    DBGPrint("debug timer deleted\n");
    return 1;
}

/* BT, BTA, BTP */

extern int bt_stack(struct task_struct *task, struct pt_regs *regs,
	            unsigned long *stack);

unsigned long backTraceHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{

    DBGPrint("bt <addr>                - display stack backtrace\n");
    DBGPrint("bta                      - display stack backtrace all pids\n");
    DBGPrint("btp <pid>                - display stack backtrace by pid\n");
    return 1;
}

unsigned long backTraceAllPID(unsigned char *cmd, StackFrame *stackFrame, unsigned long Exception,
	          DEBUGGER_PARSER *parser)
{
    struct task_struct *p, *g;

    cmd = &cmd[parser->debugCommandNameLength];
    while (*cmd && *cmd == ' ')
       cmd++;

    do_each_thread(g, p)
    {
       if (p)
       {
          DBGPrint("Stack backtrace for pid %d\n", p->pid);
          if (bt_stack(p, NULL, NULL))
             return 1;
       }
    } while_each_thread(g, p);
    return 1;
}

unsigned long backTracePID(unsigned char *cmd, StackFrame *stackFrame, unsigned long Exception,
	          DEBUGGER_PARSER *parser)
{
    int pid;
    unsigned long valid = 0;
    struct task_struct *p, *g;

    cmd = &cmd[parser->debugCommandNameLength];
    while (*cmd && *cmd == ' ')
       cmd++;

    pid = EvaluateNumericExpression(stackFrame, &cmd, &valid);
    if (valid)
    {
       do_each_thread(g, p)
       {
          if (p && (p->pid == pid))
          {
             DBGPrint("Stack backtrace for pid %d\n", p->pid);
             bt_stack(p, NULL, NULL);
             return 1;
          }
       } while_each_thread(g, p);
       DBGPrint("No process with pid %d found\n", pid);
    }
    else
       DBGPrint("invalid pid entered for backtrace\n");

    return 1;

}

unsigned long backTraceStack(unsigned char *cmd, StackFrame *stackFrame, unsigned long Exception,
	             DEBUGGER_PARSER *parser)
{
    unsigned long valid = 0, address;

    cmd = &cmd[parser->debugCommandNameLength];
    while (*cmd && *cmd == ' ')
       cmd++;

    address = EvaluateExpression(stackFrame, &cmd, &valid);
    if (valid)
    {
       DBGPrint("Stack backtrace for address 0x%08X\n", (unsigned)address);
       bt_stack(NULL, NULL, (unsigned long *)address);
       return 1;
    }
    else
    {
       DBGPrint("Stack backtrace for address 0x%08X\n",
                (unsigned)GetStackAddress(stackFrame));
       bt_stack(NULL, NULL, (unsigned long *)GetStackAddress(stackFrame));
       return 1;
    }
    return 1;
}

void DisplayASCIITable(void)
{

    register unsigned long i;
    union bhex
    {
       unsigned int i;
       struct btemp {
	     unsigned one : 1;
	     unsigned two : 1;
	     unsigned three : 1;
	     unsigned four : 1;
	     unsigned five : 1;
	     unsigned six : 1;
	     unsigned seven : 1;
	     unsigned eight : 1;
       } b;
    } val;

    DBGPrint("ASCII Table\n");
    for (i = 0; i < 256; i++)
    {
       val.i = i;
       switch (i)
       {

	  case 0:
	     if (DBGPrint("|  %3i  |  (0x%02X)  |  (%1i%1i%1i%1i%1i%1i%1i%1ib)  | NULL  |", (int)i, (unsigned)i,
		(int)val.b.eight, (int)val.b.seven, (int)val.b.six,
                (int)val.b.five,  (int)val.b.four, (int)val.b.three,
                (int)val.b.two, (int)val.b.one)) return;
	     break;

	  case 8:
	     if (DBGPrint("|  %3i  |  (0x%02X)  |  (%1i%1i%1i%1i%1i%1i%1i%1ib)  | BKSP  |", (int)i, (unsigned)i,
		(int)val.b.eight, (int)val.b.seven, (int)val.b.six,
                (int)val.b.five,  (int)val.b.four,  (int)val.b.three,
                (int)val.b.two, (int)val.b.one)) return;
	     break;

	  case 9:
	     if (DBGPrint("|  %3i  |  (0x%02X)  |  (%1i%1i%1i%1i%1i%1i%1i%1ib)  | TAB   |", (int)i, (unsigned)i,
		(int)val.b.eight, (int)val.b.seven, (int)val.b.six,
                (int)val.b.five,  (int)val.b.four,  (int)val.b.three,
                (int)val.b.two, (int)val.b.one)) return;
	     break;

	  case 10:
	     if (DBGPrint("|  %3i  |  (0x%02X)  |  (%1i%1i%1i%1i%1i%1i%1i%1ib)  | <CR>  |", (int)i, (unsigned)i,
		(int)val.b.eight, (int)val.b.seven, (int)val.b.six,
                (int)val.b.five,  (int)val.b.four,  (int)val.b.three,
                (int)val.b.two, (int)val.b.one)) return;
	     break;

	  case 13:
	     if (DBGPrint("|  %3i  |  (0x%02X)  |  (%1i%1i%1i%1i%1i%1i%1i%1ib)  | <LF>  |", (int)i, (unsigned)i,
		(int)val.b.eight, (int)val.b.seven, (int)val.b.six,
                (int)val.b.five,  (int)val.b.four,  (int)val.b.three,
                (int)val.b.two, (int)val.b.one)) return;
	     break;

	  case 32:
	     if (DBGPrint("|  %3i  |  (0x%02X)  |  (%1i%1i%1i%1i%1i%1i%1i%1ib)  | SPACE |", (int)i, (unsigned)i,
	        (int)val.b.eight, (int)val.b.seven, (int)val.b.six,
                (int)val.b.five,  (int)val.b.four,  (int)val.b.three,
                (int)val.b.two, (int)val.b.one)) return;
	     break;

	  default:
	     if (DBGPrint("|  %3i  |  (0x%02X)  |  (%1i%1i%1i%1i%1i%1i%1i%1ib)  |  %c    |", (int)i, (unsigned)i,
	        (int)val.b.eight, (int)val.b.seven, (int)val.b.six,
                (int)val.b.five,  (int)val.b.four,  (int)val.b.three,
                (int)val.b.two, (int)val.b.one, (unsigned char) i)) return;
	     break;

       }
       if (DBGPrint("\n")) return;
    }

}

#if defined(CONFIG_MODULES)

/* LSMOD, .M */

unsigned long listModulesHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint(".M                       - list loaded modules\n");
    DBGPrint("lsmod                    - list loaded modules\n");
    DBGPrint("rmmod <name>             - unload module\n");
    return 1;
}

unsigned long listModules(unsigned char *cmd, StackFrame *stackFrame, unsigned long Exception,
	          DEBUGGER_PARSER *parser)
{
    cmd = &cmd[parser->debugCommandNameLength];
    while (*cmd && *cmd == ' ')
       cmd++;

    if (*cmd)
       mdb_modules(cmd, DBGPrint);
    else
       mdb_modules(NULL, DBGPrint);
    return 1;
}

unsigned long unloadModule(unsigned char *cmd, StackFrame *stackFrame, unsigned long Exception,
	           DEBUGGER_PARSER *parser)
{
    cmd = &cmd[parser->debugCommandNameLength];
    while (*cmd && *cmd == ' ')
       cmd++;

    DBGPrint("Module unload unsupported in this version\n");
    return 0;
}

#endif

/* REBOOT */

unsigned long rebootSystemHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("reboot                   - reboot host system\n");
    DBGPrint("reboot force             - reboot from current processor\n");
    return 1;
}


unsigned long rebootSystem(unsigned char *cmd, StackFrame *stackFrame, unsigned long Exception,
	     DEBUGGER_PARSER *parser)
{
    extern void machine_emergency_restart(void);

    cmd = &cmd[parser->debugCommandNameLength];
    while (*cmd && *cmd == ' ')
       cmd++;

    if (!strnicmp(cmd, "force", 5))
    {
       machine_emergency_restart();
       return 1;
    }

    if (!get_processor_id())
       machine_emergency_restart();
    else
       DBGPrint("not on processor 0.  try 'reboot force' or switch to \n"
                "processor 0 with 'cpu 0' or 'nmi 0' commands and issue\n"
                "the reboot command again\n");
    return 1;
}

/* SECTIONS, .S */

unsigned long displaySectionsHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("sections                 - display kernel/module sections\n");
    DBGPrint(".s                       - display kernel/module sections\n");
    return 1;
}

unsigned long displaySections(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     DBGPrint("\n");
     return 1;
}

/* PS, .P */

unsigned long displayKernelProcessHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    if (commandLine) {}
    if (parser) {}

    DBGPrint("ps <addr>                - display kernel processes\n");
    DBGPrint(".p <addr>                - display kernel processes\n");
    return 1;
}

unsigned long displayKernelProcess(unsigned char *cmd,
		           StackFrame *stackFrame, unsigned long Exception,
		           DEBUGGER_PARSER *parser)
{
     struct task_struct *p, *g;
     unsigned long valid = 0;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (*cmd)
     {
        p = (struct task_struct *)EvaluateExpression(stackFrame, &cmd, &valid);
        if (valid && p)
        {
	   DBGPrint("%-*s      Pid   Parent [*] State %-*s Command\n",
		     (int)(2*sizeof(void *))+2, "Task Addr",
		     (int)(2*sizeof(void *))+2, "Thread");

	   if (DBGPrint("0x%p %8d %8d  %d    %c  0x%p %s\n",
	       (void *)p, p->pid, p->real_parent->pid,
               p == current,
               (p->state == 0) ? 'R' :
	       (p->state < 0) ? 'U' :
	       (p->state & TASK_UNINTERRUPTIBLE) ? 'D' :
	       (p->state & TASK_STOPPED) ? 'T' :
	       (p->state & TASK_TRACED) ? 'C' :
	       (p->exit_state & EXIT_ZOMBIE) ? 'Z' :
	       (p->exit_state & EXIT_DEAD) ? 'E' :
	       (p->state & TASK_INTERRUPTIBLE) ? 'S' : '?',
	       (void *)(&p->thread),
	        p->comm))
                  return 1;
            return 1;
        }
        DBGPrint("invalid task address\n");
        return 1;
     }
     else
     {
	DBGPrint("%-*s      Pid   Parent [*] State %-*s Command\n",
		(int)(2*sizeof(void *))+2, "Task Addr",
		(int)(2*sizeof(void *))+2, "Thread");

        do_each_thread(g, p)
        {
           if (p)
           {
	      if (DBGPrint("0x%p %8d %8d  %d    %c  0x%p %s\n",
		   (void *)p, p->pid, p->real_parent->pid,
                   p == current,
                   (p->state == 0) ? 'R' :
		   (p->state < 0) ? 'U' :
		   (p->state & TASK_UNINTERRUPTIBLE) ? 'D' :
		   (p->state & TASK_STOPPED) ? 'T' :
		   (p->state & TASK_TRACED) ? 'C' :
		   (p->exit_state & EXIT_ZOMBIE) ? 'Z' :
		   (p->exit_state & EXIT_DEAD) ? 'E' :
		   (p->state & TASK_INTERRUPTIBLE) ? 'S' : '?',
		   (void *)(&p->thread),
		   p->comm))
                 return 1;
           }
        } while_each_thread(g, p);
     }
     return 1;

}

unsigned long ascTableHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("a                        - display ASCII Table\n");
    return 1;
}

/* A */

unsigned long displayASCTable(unsigned char *cmd,
		     StackFrame *stackFrame, unsigned long Exception,
		     DEBUGGER_PARSER *parser)
{
     DisplayASCIITable();
     return 1;
}

typedef struct _LINE_INFO
{
   unsigned long SourcePresent;
   unsigned char *SourceLine;
   unsigned char *ModuleName;
   unsigned long LineNumber;
} LINE_INFO;

void GetLineInfoFromValue(unsigned long value, LINE_INFO *lineInfo, unsigned long *exact)
{
    if (exact)
       *exact = 0;

    if (lineInfo)
    {
        lineInfo->SourcePresent = 0;
        lineInfo->SourceLine = "";
        lineInfo->ModuleName = "";
        lineInfo->LineNumber = 0;
    }
    return;
}

unsigned long disassemble(StackFrame *stackFrame, unsigned long p, unsigned long count, unsigned long use)
{
    register unsigned long i;
    unsigned char *symbolName;
    unsigned char *moduleName;
    unsigned long exact = 0;
    extern unsigned long line_info_toggle;
    LINE_INFO lineInfo;
    register int c = get_processor_id();

    for (i = 0; i < count; i++)
    {
       GetLineInfoFromValue(p, &lineInfo, &exact);

       if (line_info_toggle && exact)
       {
	  if (lineInfo.SourcePresent && lineInfo.SourceLine)
	  {
	     register unsigned long length = strlen(lineInfo.SourceLine);

	     i = length > 80
             ? i + 1 + (length / 80)
             : i + 1;

	     DBGPrint("%s (%s : line %d)\n",
				 lineInfo.SourceLine, lineInfo.ModuleName,
				 lineInfo.LineNumber);

	  }
	  else if (line_info_toggle && lineInfo.LineNumber)
	  {
	     i++;
	     DBGPrint("file %s  line %d\n",
				 lineInfo.ModuleName, lineInfo.LineNumber);
	  }
       }

       if (i >= count && count != 1)
	  break;

       symbolName = GetSymbolFromValue(p, &symbuf[c][0], MAX_SYMBOL_LEN);
       if (symbolName)
       {
	  i++;
          moduleName = GetModuleInfoFromSymbolValue(p, &modbuf[c][0],
                                                    MAX_SYMBOL_LEN);
          if (moduleName)
          {
	     if (DBGPrint("%s|%s:\n", moduleName, symbolName)) return p;
          }
          else
          {
	     if (DBGPrint("%s:\n", symbolName)) return p;
          }
       }
       if (i >= count && count != 1)
	  break;

       if (unassemble(stackFrame, p, use, &p)) return p;
    }

    return p;

}

unsigned long dumpSearchResults(unsigned char *p, unsigned long count)
{

   unsigned char *symbolName;
   unsigned char *moduleName;
   register unsigned long i, r, total;
   unsigned char ch;
   register int c = get_processor_id();

   if (DBGPrint("           0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n"))
      return 1;

   for (r = 0; r < count; r++)
   {
      symbolName = GetSymbolFromValue((unsigned long) p, &symbuf[c][0], MAX_SYMBOL_LEN);
      if (symbolName)
      {
         moduleName = GetModuleInfoFromSymbolValue((unsigned long) p, &modbuf[c][0],
                                                   MAX_SYMBOL_LEN);
         if (moduleName)
         {
	    if (DBGPrint("%s|%s:\n", moduleName, symbolName)) return 1;
         }
         else
         {
	    if (DBGPrint("%s:\n", symbolName)) return 1;
         }
	 if (r++ >= count && count != 1)
	    break;
      }
      DBGPrint("%08X ", (unsigned) p);
      for (total = 0, i = 0; i < 16; i++, total++)
      {
	 DBGPrint(" %02X", (unsigned) mdb_getword((unsigned long)&p[i], 1));
      }
      DBGPrint("  ");
      for (i = 0; i < total; i++)
      {
         ch = mdb_getword((unsigned long)&p[i], 1);

	 if (ch < 32 || ch > 126) DBGPrint(".");
	 else DBGPrint("%c", ch);
      }
      if (DBGPrint("\n")) return 1;

      p = (void *)((unsigned long) p + (unsigned long) total);
   }
   return 0;

}

unsigned char *dump(unsigned char *p, unsigned long count)
{

   unsigned char *symbolName;
   unsigned char *moduleName;
   register unsigned long i, r, total;
   unsigned char ch;
   register int c = get_processor_id();

   DBGPrint("           0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");

   for (r = 0; r < count; r++)
   {
      symbolName = GetSymbolFromValue((unsigned long) p, &symbuf[c][0], MAX_SYMBOL_LEN);
      if (symbolName)
      {
         moduleName = GetModuleInfoFromSymbolValue((unsigned long) p, &modbuf[c][0],
                                                   MAX_SYMBOL_LEN);
         if (moduleName)
	    DBGPrint("%s|%s:\n", moduleName, symbolName);
         else
	    DBGPrint("%s:\n", symbolName);
	 if (r++ >= count && count != 1)
	    break;
      }
      DBGPrint("%08X ", (unsigned) p);
      for (total = 0, i = 0; i < 16; i++, total++)
      {
	 DBGPrint(" %02X", (unsigned) mdb_getword((unsigned long)&p[i], 1));
      }
      DBGPrint("  ");
      for (i = 0; i < total; i++)
      {
         ch = mdb_getword((unsigned long)&p[i], 1);

	 if (ch < 32 || ch > 126) DBGPrint(".");
	 else DBGPrint("%c", ch);
      }
      DBGPrint("\n");

      p = (void *)((unsigned long) p + (unsigned long) total);
   }

   return p;

}

unsigned long dumpWordSearchResults(unsigned char *p, unsigned long count)
{

   register int i, r;
   unsigned short *wp;
   unsigned char *symbolName;
   unsigned char *moduleName;
   unsigned char ch;
   register int c = get_processor_id();

   wp = (unsigned short *) p;
   for (r = 0; r < count; r++)
   {
      symbolName = GetSymbolFromValue((unsigned long) p, &symbuf[c][0], MAX_SYMBOL_LEN);
      if (symbolName)
      {
         moduleName = GetModuleInfoFromSymbolValue((unsigned long) p, &modbuf[c][0],
                                                   MAX_SYMBOL_LEN);
         if (moduleName)
         {
	    if (DBGPrint("%s|%s:\n", moduleName, symbolName)) return 1;
         }
         else
         {
	    if (DBGPrint("%s:\n", symbolName)) return 1;
         }
	 if (r++ >= count && count != 1)
	    break;
      }
      DBGPrint("%08X ", (unsigned) p);
      for (i = 0; i < (16 / 2); i++)
      {
	 DBGPrint(" %04X", (unsigned) mdb_getword((unsigned long)&wp[i], 2));
      }
      DBGPrint("  ");
      for (i = 0; i < 16; i++)
      {
         ch = mdb_getword((unsigned long)&p[i], 1);

	 if (ch < 32 || ch > 126) DBGPrint(".");
	 else DBGPrint("%c", ch);
      }
      if (DBGPrint("\n")) return 1;

      p = (void *)((unsigned long) p + (unsigned long) 16);
      wp = (unsigned short *) p;
   }

   return 0;

}

unsigned char *dumpWord(unsigned char *p, unsigned long count)
{

   register int i, r;
   unsigned short *wp;
   unsigned char *symbolName;
   unsigned char *moduleName;
   unsigned char ch;
   register int c = get_processor_id();

   wp = (unsigned short *) p;
   for (r = 0; r < count; r++)
   {
      symbolName = GetSymbolFromValue((unsigned long) p, &symbuf[c][0], MAX_SYMBOL_LEN);
      if (symbolName)
      {
         moduleName = GetModuleInfoFromSymbolValue((unsigned long) p, &modbuf[c][0],
                                                   MAX_SYMBOL_LEN);
         if (moduleName)
	    DBGPrint("%s|%s:\n", moduleName, symbolName);
         else
	    DBGPrint("%s:\n", symbolName);
	 if (r++ >= count && count != 1)
	    break;
      }
      DBGPrint("%08X ", (unsigned) p);
      for (i = 0; i < (16 / 2); i++)
      {
	 DBGPrint(" %04X", (unsigned) mdb_getword((unsigned long)&wp[i], 2));
      }
      DBGPrint("  ");
      for (i = 0; i < 16; i++)
      {
         ch = mdb_getword((unsigned long)&p[i], 1);

	 if (ch < 32 || ch > 126) DBGPrint(".");
	 else DBGPrint("%c", ch);
      }
      DBGPrint("\n");

      p = (void *)((unsigned long) p + (unsigned long) 16);
      wp = (unsigned short *) p;
   }

   return p;

}

unsigned long dumpDoubleSearchResults(unsigned char *p, unsigned long count)
{

   register int i, r;
   unsigned long *lp;
   unsigned char *symbolName;
   unsigned char *moduleName;
   unsigned char ch;
   register int c = get_processor_id();

   lp = (unsigned long *) p;

   for (r = 0; r < count; r++)
   {
      symbolName = GetSymbolFromValue((unsigned long) p, &symbuf[c][0], MAX_SYMBOL_LEN);
      if (symbolName)
      {
         moduleName = GetModuleInfoFromSymbolValue((unsigned long) p, &modbuf[c][0],
                                                   MAX_SYMBOL_LEN);
         if (moduleName)
         {
	    if (DBGPrint("%s|%s:\n", moduleName, symbolName)) return 1;
         }
         else
         {
	    if (DBGPrint("%s:\n", symbolName)) return 1;
         }
	 if (r++ >= count && count != 1)
	    break;
      }
      DBGPrint("%08X ", (unsigned) p);
      for (i = 0; i < (16 / 4); i++)
      {
	 DBGPrint(" %08X", (unsigned) mdb_getword((unsigned long)&lp[i], 4));
      }
      DBGPrint("  ");
      for (i = 0; i < 16; i++)
      {
         ch = mdb_getword((unsigned long)&p[i], 1);

	 if (ch < 32 || ch > 126) DBGPrint(".");
	 else DBGPrint("%c", ch);
      }
      if (DBGPrint("\n")) return 1;

      p = (void *)((unsigned long) p + (unsigned long) 16);
      lp = (unsigned long *) p;
   }

   return 0;

}

unsigned char *dumpDouble(unsigned char *p, unsigned long count)
{

   register int i, r;
   unsigned long *lp;
   unsigned char *symbolName;
   unsigned char *moduleName;
   unsigned char ch;
   register int c = get_processor_id();

   lp = (unsigned long *) p;

   for (r = 0; r < count; r++)
   {
      symbolName = GetSymbolFromValue((unsigned long) p, &symbuf[c][0], MAX_SYMBOL_LEN);
      if (symbolName)
      {
         moduleName = GetModuleInfoFromSymbolValue((unsigned long) p, &modbuf[c][0],
                                                   MAX_SYMBOL_LEN);
         if (moduleName)
	    DBGPrint("%s|%s:\n", moduleName, symbolName);
         else
	    DBGPrint("%s:\n", symbolName);
	 if (r++ >= count && count != 1)
	    break;
      }
      DBGPrint("%08X ", (unsigned) p);
      for (i = 0; i < (16 / 4); i++)
      {
	 DBGPrint(" %08X", (unsigned) mdb_getword((unsigned long)&lp[i], 4));
      }
      DBGPrint("  ");
      for (i = 0; i < 16; i++)
      {
         ch = mdb_getword((unsigned long)&p[i], 1);

	 if (ch < 32 || ch > 126) DBGPrint(".");
	 else DBGPrint("%c", ch);
      }
      DBGPrint("\n");

      p = (void *)((unsigned long) p + (unsigned long) 16);
      lp = (unsigned long *) p;
   }

   return p;

}

unsigned char *dumpLinkedList(unsigned char *p, unsigned long count, unsigned long offset)
{

   register int i, r;
   unsigned long *lp;
   unsigned char ch;

   lp = (unsigned long *) p;

   DBGPrint("           0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
   DBGPrint("Linked List -> [%08X + %X] = %08X\n", (unsigned)lp,
            (unsigned)offset,
            (unsigned)mdb_getword((unsigned long)((unsigned long)lp + (unsigned long)offset), 4));

   for (r = 0; r < count; r++)
   {
      DBGPrint("%08X ", (unsigned) p);
      for (i = 0; i < 16; i++)
      {
	 DBGPrint(" %02X", (unsigned) mdb_getword((unsigned long)&p[i], 1));
      }
      DBGPrint("  ");
      for (i = 0; i < 16; i++)
      {
         ch = mdb_getword((unsigned long)&p[i], 1);

	 if (ch < 32 || ch > 126) DBGPrint(".");
	 else DBGPrint("%c", ch);
      }
      DBGPrint("\n");

      p = (void *)((unsigned long) p + (unsigned long) 16);
   }

   return (unsigned char *)(mdb_getword((unsigned long)((unsigned long)lp + (unsigned long)offset), 4));

}

unsigned char *dumpDoubleStack(StackFrame *stackFrame, unsigned char *p, unsigned long count)
{

   register int i, r;
   unsigned long *lp;
   unsigned char ch;

   lp = (unsigned long *) p;

   DBGPrint("Stack = %04lX:%08X\n",
            (unsigned long)GetStackSegment(stackFrame),
            (unsigned)p);

   for (r = 0; r < count; r++)
   {
      DBGPrint("%04X:", (unsigned) GetStackSegment(stackFrame));
      DBGPrint("%08X ", (unsigned) p);
      for (i = 0; i < (16 / 4); i++)
      {
	 DBGPrint(" %08X", (unsigned) mdb_getword((unsigned long)&lp[i], 4));
      }
      DBGPrint("  ");
      for (i = 0; i < 16; i++)
      {
         ch = mdb_getword((unsigned long)&p[i], 1);

	 if (ch < 32 || ch > 126) DBGPrint(".");
	 else DBGPrint("%c", ch);
      }
      DBGPrint("\n");

      p = (void *)((unsigned long) p + (unsigned long) 16);
      lp = (unsigned long *) p;
   }

   return p;

}

unsigned char *dumpStack(StackFrame *stackFrame, unsigned char *p, unsigned long count)
{

   register int r;
   unsigned long *lp;

   lp = (unsigned long *) p;

   DBGPrint("Stack = %04X:%08X\n", (unsigned)GetStackSegment(stackFrame),
            (unsigned)p);

   for (r = 0; r < count; r++)
   {
      DBGPrint("%08X ", (unsigned) p);
      DBGPrint("%08X ", (unsigned) mdb_getword((unsigned long)lp, 4));
      if (DisplayClosestSymbol(mdb_getword((unsigned long)lp, 4)))
         DBGPrint("\n");

      p = (void *)((unsigned long) p + (unsigned long) 4);
      lp = (unsigned long *) p;
   }

   return p;

}

unsigned long displayToggleHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint(".tc                      - toggles control registers (ON | OFF)\n");
    DBGPrint(".tn                      - toggles coprocessor registers (ON | OFF)\n");
    DBGPrint(".ts                      - toggles segment registers (ON | OFF)\n");
    DBGPrint(".tg                      - toggles general registers (ON | OFF)\n");
    DBGPrint(".tr                      - toggles display of break reason (ON | OFF)\n");
    DBGPrint(".td                      - toggles full dereference display (ON | OFF)\n");
    DBGPrint(".tl                      - toggles source line display (ON | OFF)\n");
    DBGPrint(".tu                      - toggles unasm debug display (ON | OFF)\n");
    DBGPrint(".t or .t <address>       - display task state segment (tss)\n");
    return 1;
}

/* .TU */

unsigned long ProcessTUToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     (debug_deref)
     ? (debug_deref = 0)
     : (debug_deref = 1);
     DBGPrint("toggle unasm debug display (%s)\n",
				  debug_deref ? "ON" : "OFF");
     return 1;
}

/* .TD */

unsigned long ProcessTDToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     (full_deref_toggle)
     ? (full_deref_toggle = 0)
     : (full_deref_toggle = 1);
     DBGPrint("toggle full dereferencing info (%s) \n",
					    full_deref_toggle ? "ON" : "OFF");
     return 1;
}


/* .TL */

unsigned long ProcessTLToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     (line_info_toggle)
     ? (line_info_toggle = 0)
     : (line_info_toggle = 1);
     DBGPrint("toggle source line info (%s) \n",
					    line_info_toggle ? "ON" : "OFF");
     return 1;

}

/* .TG */

unsigned long ProcessTGToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     (general_toggle)
     ? (general_toggle = 0)
     : (general_toggle = 1);
     DBGPrint("toggle general registers (%s) \n",
					    general_toggle ? "ON" : "OFF");
     return 1;

}

/* .TC */

unsigned long ProcessTCToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     (control_toggle)
     ? (control_toggle = 0)
     : (control_toggle = 1);
     DBGPrint("toggle control registers (%s) \n",
					    control_toggle ? "ON" : "OFF");
     return 1;

}

/* .TN */

unsigned long ProcessTNToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     (numeric_toggle)
     ? (numeric_toggle = 0)
     : (numeric_toggle = 1);
     DBGPrint("toggle coprocessor registers (%s) \n",
					    numeric_toggle ? "ON" : "OFF");
     return 1;

}

/* .TR */

unsigned long ProcessTRToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     (reason_toggle)
     ? (reason_toggle = 0)
     : (reason_toggle = 1);
     DBGPrint("toggle display break reason (%s) \n",
					    reason_toggle ? "ON" : "OFF");
     return 1;

}

/* .TS */

unsigned long ProcessTSToggle(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     (segment_toggle)
     ? (segment_toggle = 0)
     : (segment_toggle = 1);
     DBGPrint("toggle segment registers (%s) \n",
					    segment_toggle ? "ON" : "OFF");
     return 1;

}

unsigned long displayDebuggerVersionHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint(".v                       - display version info\n");
    return 1;
}

/* .V */

unsigned long DisplayDebuggerVersion(unsigned char *cmd,
		   StackFrame *stackFrame, unsigned long Exception,
		   DEBUGGER_PARSER *parser)
{
     extern unsigned long MajorVersion;
     extern unsigned long MinorVersion;
     extern unsigned long BuildVersion;

     DBGPrint("Merkey's Kernel Debugger\n");
     DBGPrint("v%02d.%02d.%02d\n",
              (int)MajorVersion, (int)MinorVersion, (int)BuildVersion);
     DBGPrint("Copyright (C) 2008 Jeffrey Vernon Merkey.  "
              "All Rights Reserved.\n");

     return 1;
}

/* .Z */

unsigned long displaySymbolsHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint(".z <name>                  - display symbol info\n");
    return 1;
}

unsigned long displaySymbols(unsigned char *cmd,
		    StackFrame *stackFrame, unsigned long Exception,
		    DEBUGGER_PARSER *parser)
{
     extern void DumpOSSymbolTableMatch(unsigned char *);

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (*cmd)
	DumpOSSymbolTableMatch(cmd);
     else
	DumpOSSymbolTableMatch(NULL);

     return 1;
}

/* LCPU */

unsigned long listProcessors(unsigned char *cmd,
		    StackFrame *stackFrame, unsigned long Exception,
		    DEBUGGER_PARSER *parser)
{
     register int i;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     DBGPrint("Current Processor: %d\n", get_processor_id());
     DBGPrint("Active Processors: \n");
     for (i = 0; i < MAX_PROCESSORS; i++)
     {
        if (cpu_online(i))
	   DBGPrint("   Processor %d\n", i);
     }
     return 1;

}

unsigned long clearScreenHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("cls                      - clear the screen\n");
    return 1;
}

/* CLS */

unsigned long clearDebuggerScreen(unsigned char *cmd,
			 StackFrame *stackFrame, unsigned long Exception,
			 DEBUGGER_PARSER *parser)
{
     extern void ClearScreen(void);

     ClearScreen();
     return 1;

}

unsigned long SearchMemoryHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("sb                       - search for bytes at address\n");
    DBGPrint("sw                       - search for words at address\n");
    DBGPrint("sd                       - search for dwords at address\n");
    return 1;
}

/* S */

/* use local storage and reduce stack space use.  these functions are always
  called single threaded from the console */

unsigned char s_changeBuffer[16];
unsigned char b_searchBuffer[16];
unsigned char b_copyBuffer[16];
unsigned short w_searchBuffer[16];
unsigned short w_copyBuffer[16];
unsigned long d_searchBuffer[16];
unsigned long d_copyBuffer[16];

unsigned long SearchMemory(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     unsigned char *changeBuffer = s_changeBuffer;
     unsigned char *searchBuffer = b_searchBuffer;
     unsigned char *copyBuffer = b_copyBuffer;
     unsigned long maxlen = sizeof(searchBuffer);
     register unsigned char *changeB;
     unsigned char *pB;
     register unsigned long address, r, value, count, len, i;
     unsigned long valid, EndingAddress = (unsigned long)high_memory;
     register int key;
     extern int mdb_getkey(void);

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     memset((unsigned long *)searchBuffer, 0, sizeof(searchBuffer));
     count = 0;
     changeB = (unsigned char *) searchBuffer;
     changeBuffer[0] = '\0';
     DBGPrint("enter bytes to search for, '.' to end input\n");
     while ((changeBuffer[0] != '.') && (count < maxlen))
     {
	for (r = 0; r < 8; r++)
	{
	   DBGPrint("0x");

	   ScreenInputFromKeyboard(&changeBuffer[0], 0, 4);

	   if ((changeBuffer[0] == '.') || (changeBuffer[1] == '.'))
	      break;

	   pB = (unsigned char *) &changeBuffer[0];
	   len = strlen(pB);

	   for (i = 0; i < len; i++)
	      DBGPrint("\b");

	   value = EvaluateExpression(0, &pB, &valid);
	   if (valid)
	      *changeB = (unsigned char) value;
	   DBGPrint("%02X ", (unsigned char) *changeB);

	   changeB++;
	   if (count++ > maxlen)
	      break;
	}
	if (DBGPrint("\n")) return 1;
     }

     if (count)
     {
	DBGPrint("enter start address for search:  ");
	ScreenInputFromKeyboard(&changeBuffer[0], 0, 16);
	pB = (unsigned char *) &changeBuffer[0];
	address = EvaluateExpression(0, &pB, &valid);
	if (valid)
	{
	   register unsigned long temp;

	   DBGPrint("start address = [%08X]\n", (unsigned)address);
	   DBGPrint("enter ending address for search:  ");

	   ScreenInputFromKeyboard(&changeBuffer[0], 0, 16);
	   pB = (unsigned char *) &changeBuffer[0];
	   temp = EvaluateExpression(0, &pB, &valid);
	   if (valid)
	      EndingAddress = temp;

	   DBGPrint("\nsearching memory from 0x%08X to 0x%08X\n",
                    (unsigned)address, (unsigned)EndingAddress);
	   while (address < EndingAddress)
	   {
              read_memory((void *)address, copyBuffer, count);
	      if (!memcmp(searchBuffer, copyBuffer, count))
	      {
		 if (DBGPrint("match at address [%08X]\n",
                     (unsigned)address)) return 1;
		 if (dumpSearchResults((unsigned char *)address, 4)) return 1;
		 if (DBGPrint("searching\n")) return 1;
	      }
	      address++;
	      if (!(address % 0x100000))
	      {
		  if (DBGPrint("searching memory at address 0x%08X ..."
                        " Q or q to abort - any key to proceed\n",
                                (unsigned)address)) return 1;
                  key = mdb_getkey();
                  if (((char)key == 'Q') || ((char)key == 'q'))
                     break;
	      }
	   }
	   if (DBGPrint("search completed.\n")) return 1;
	   return 1;
	}
	if (DBGPrint("invalid start address\n")) return 1;
	return 1;
     }
     if (DBGPrint("no search pattern\n")) return 1;
     return 1;

}

/* SB */

unsigned long SearchMemoryB(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     unsigned char *changeBuffer = s_changeBuffer;
     unsigned char *searchBuffer = b_searchBuffer;
     unsigned char *copyBuffer = b_copyBuffer;
     unsigned long maxlen = sizeof(searchBuffer);
     register unsigned char *changeB;
     unsigned char *pB;
     register unsigned long address, r, value, count, len, i;
     unsigned long valid, EndingAddress = (unsigned long)high_memory;
     register int key;
     extern int mdb_getkey(void);

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     memset((unsigned long *)searchBuffer, 0, sizeof(searchBuffer));
     count = 0;
     changeB = (unsigned char *) searchBuffer;
     changeBuffer[0] = '\0';
     DBGPrint("enter bytes to search for, '.' to end input\n");
     while (changeBuffer[0] != '.' && count < maxlen)
     {
	for (r = 0; r < 8; r++)
	{
	   DBGPrint("0x");

	   ScreenInputFromKeyboard(&changeBuffer[0], 0, 4);

	   if ((changeBuffer[0] == '.') || (changeBuffer[1] == '.'))
	      break;

	   pB = (unsigned char *) &changeBuffer[0];
	   len = strlen(pB);
	   for (i = 0; i < len; i++)
	      DBGPrint("\b");

	   value = EvaluateExpression(0, &pB, &valid);
	   if (valid)
	      *changeB = (unsigned char) value;
	   DBGPrint("%02X ", (unsigned char) *changeB);

	   changeB++;
	   if (count++ > maxlen)
	      break;
	}
	if (DBGPrint("\n")) return 1;
     }

     if (count)
     {
	DBGPrint("enter start address for search:  ");
	ScreenInputFromKeyboard(&changeBuffer[0], 0, 16);
	pB = (unsigned char *) &changeBuffer[0];
	address = EvaluateExpression(0, &pB, &valid);
	if (valid)
	{
	   register unsigned long temp;

	   DBGPrint("start address = [%08X]\n", (unsigned)address);

	   DBGPrint("enter ending address for search:  ");
	   ScreenInputFromKeyboard(&changeBuffer[0], 0, 16);
	   pB = (unsigned char *) &changeBuffer[0];
	   temp = EvaluateExpression(0, &pB, &valid);
	   if (valid)
	      EndingAddress = temp;

	   DBGPrint("\nsearching memory from 0x%08X to 0x%08X\n",
                    (unsigned)address, (unsigned)EndingAddress);
	   while (address < EndingAddress)
	   {
              read_memory((void *)address, copyBuffer, count);
	      if (!memcmp(searchBuffer, copyBuffer, count))
	      {
		 if (DBGPrint("match at address [%08X]\n",
                     (unsigned)address)) return 1;
		 if (dumpSearchResults((unsigned char *)address, 4)) return 1;
		 if (DBGPrint("searching\n")) return 1;
	      }
	      address++;
	      if (!(address % 0x100000))
	      {
		 if (DBGPrint("searching memory at address 0x%08X ..."
                        " Q or q to abort - any key to proceed\n",
                              (unsigned)address)) return 1;
                 key = mdb_getkey();
                 if (((char)key == 'Q') || ((char)key == 'q'))
                    break;
	      }
	   }
	   if (DBGPrint("search completed.\n")) return 1;
	   return 1;
	}
	if (DBGPrint("invalid start address\n")) return 1;
	return 1;
     }
     if (DBGPrint("no search pattern\n")) return 1;
     return 1;
}

/* SW */

unsigned long SearchMemoryW(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     unsigned char *changeBuffer = s_changeBuffer;
     unsigned short *searchBuffer = w_searchBuffer;
     unsigned short *copyBuffer = w_copyBuffer;
     unsigned long maxlen = sizeof(searchBuffer) / sizeof(unsigned short);
     register unsigned short *changeW;
     unsigned char *pB;
     register unsigned long address, r, value, count, len, i;
     unsigned long valid, EndingAddress = (unsigned long)high_memory;
     register int key;
     extern int mdb_getkey(void);

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     memset((unsigned long *)searchBuffer, 0, sizeof(searchBuffer));
     count = 0;
     changeW = (unsigned short *) searchBuffer;
     changeBuffer[0] = '\0';
     DBGPrint("enter words to search for, '.' to end input\n");
     while (changeBuffer[0] != '.' && count < maxlen)
     {
	for (r = 0; r < 4; r++)
	{
	   DBGPrint("0x");

	   ScreenInputFromKeyboard(&changeBuffer[0], 0, 6);

	   if ((changeBuffer[0] == '.') || (changeBuffer[1] == '.') ||
	       (changeBuffer[2] == '.') || (changeBuffer[3] == '.'))
	      break;

	   pB = (unsigned char *) &changeBuffer[0];
	   len = strlen(pB);
	   for (i = 0; i < len; i++)
	      DBGPrint("\b");

	   value = EvaluateExpression(0, &pB, &valid);
	   if (valid)
	      *changeW = value;
	   DBGPrint("%04X ", *changeW);

	   changeW++;
	   if (count++ > maxlen)
	      break;
	}
	if (DBGPrint("\n")) return 1;
     }

     if (count)
     {
	DBGPrint("enter start address for search:  ");
	ScreenInputFromKeyboard(&changeBuffer[0], 0, 16);
	pB = (unsigned char *) &changeBuffer[0];
	address = EvaluateExpression(0, &pB, &valid);
	if (valid)
	{
	   register unsigned long temp;

	   DBGPrint("start address = [%08X]\n", (unsigned)address);

	   DBGPrint("enter ending address for search:  ");
	   ScreenInputFromKeyboard(&changeBuffer[0], 0, 16);
	   pB = (unsigned char *) &changeBuffer[0];
	   temp = EvaluateExpression(0, &pB, &valid);
	   if (valid)
	      EndingAddress = temp;

	   DBGPrint("searching memory from 0x%08X to 0x%08X\n",
                    (unsigned)address, (unsigned)EndingAddress);
	   while (address < EndingAddress)
	   {
              read_memory((void *)address, copyBuffer, count * sizeof(unsigned short));
	      if (!memcmp(searchBuffer, copyBuffer, count * sizeof(unsigned short)))
	      {
		 if (DBGPrint("match at address [%08X]\n",
                     (unsigned)address)) return 1;
		 if (dumpWordSearchResults((unsigned char *)address, 4))
                     return 1;
		 if (DBGPrint("searching\n")) return 1;;
	      }
	      address++;
	      if (!(address % 0x100000))
	      {
		 if (DBGPrint("searching memory at address 0x%08X ..."
                        " Q or q to abort - any key to proceed\n",
                              (unsigned)address)) return 1;
                  key = mdb_getkey();
                  if (((char)key == 'Q') || ((char)key == 'q'))
                     break;
	      }
	   }
	   if (DBGPrint("search completed.\n")) return 1;
	   return 1;
	}
	if (DBGPrint("invalid start address\n")) return 1;
	return 1;
     }
     if (DBGPrint("no search pattern\n")) return 1;
     return 1;
}

/* SD */

unsigned long SearchMemoryD(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     register unsigned char *changeBuffer = s_changeBuffer;
     register unsigned long *searchBuffer = d_searchBuffer;
     register unsigned long *copyBuffer = d_copyBuffer;
     register unsigned long maxlen = sizeof(searchBuffer) / sizeof(unsigned long);
     register unsigned long *changeD;
     unsigned char *pB;
     register unsigned long address, r, value, count, len, i;
     unsigned long valid, EndingAddress = (unsigned long)high_memory;
     register int key;
     extern int mdb_getkey(void);

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     memset((unsigned long *)searchBuffer, 0, sizeof(searchBuffer));
     count = 0;
     changeD = (unsigned long *) searchBuffer;
     changeBuffer[0] = '\0';
     DBGPrint("enter dwords to search for, '.' to end input\n");
     while (changeBuffer[0] != '.' && count < maxlen)
     {
	for (r = 0; r < 2; r++)
	{
	   DBGPrint("0x");

	   ScreenInputFromKeyboard(&changeBuffer[0], 0, 8);

	   if ((changeBuffer[0] == '.') || (changeBuffer[1] == '.') ||
	       (changeBuffer[2] == '.') || (changeBuffer[3] == '.') ||
	       (changeBuffer[4] == '.') || (changeBuffer[5] == '.') ||
	       (changeBuffer[6] == '.') || (changeBuffer[7] == '.'))
	      break;

	   pB = (unsigned char *) &changeBuffer[0];
	   len = strlen(pB);
	   for (i = 0; i < len; i++)
	      DBGPrint("\b");

	   value = EvaluateExpression(0, &pB, &valid);
	   if (valid)
	      *changeD = value;
	   DBGPrint("%08X ", (unsigned)*changeD);

	   changeD++;
	   if (count++ > maxlen)
	      break;
	}
	if (DBGPrint("\n")) return 1;
     }

     if (count)
     {
	DBGPrint("enter start address for search:  ");
	ScreenInputFromKeyboard(&changeBuffer[0], 0, 16);
	pB = (unsigned char *) &changeBuffer[0];
	address = EvaluateExpression(0, &pB, &valid);
	if (valid)
	{
	   register unsigned long temp;

	   DBGPrint("start address = [%08X]\n", (unsigned)address);

	   DBGPrint("enter ending address for search:  ");
	   ScreenInputFromKeyboard(&changeBuffer[0], 0, 16);
	   pB = (unsigned char *) &changeBuffer[0];
	   temp = EvaluateExpression(0, &pB, &valid);
	   if (valid)
	      EndingAddress = temp;

	   DBGPrint("searching memory from 0x%08X to 0x%08X\n",
                    (unsigned)address, (unsigned)EndingAddress);
	   while (address < EndingAddress)
	   {
              read_memory((void *)address, copyBuffer, count * sizeof(unsigned long));
	      if (!memcmp(searchBuffer, copyBuffer, count * sizeof(unsigned long)))
	      {
		 if (DBGPrint("match at address [%08X]\n",
                     (unsigned)address)) return 1;
		 if (dumpDoubleSearchResults((unsigned char *)address, 4))
                     return 1;
		 if (DBGPrint("searching\n")) return 1;
	      }
	      address++;
	      if (!(address % 0x100000))
	      {
		 if (DBGPrint("searching memory at address 0x%08X ..."
                        " Q or q to abort - any key to proceed\n",
                              (unsigned)address)) return 1;
                  key = mdb_getkey();
                  if (((char)key == 'Q') || ((char)key == 'q'))
                     break;
	      }
	   }
	   if (DBGPrint("search completed.\n")) return 1;
	   return 1;
	}
	if (DBGPrint("invalid start address\n")) return 1;
	return 1;
     }
     if (DBGPrint("no search pattern\n")) return 1;
     return 1;
}


unsigned long changeMemoryHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("c   <address>            - change bytes at address\n");
    DBGPrint("cb  <address>            - change bytes at address\n");
    DBGPrint("cw  <address>            - change words at address\n");
    DBGPrint("cd  <address>            - change dwords at address\n");
    return 1;
}

/* CW */

unsigned long changeWordValue(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     register unsigned char *changeBuffer = &workbuf[0][0];
     register unsigned short *changeW, oldW;
     unsigned char *pB;
     register unsigned long address, r, value, len, i;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	changeW = (unsigned short *) address;
	changeBuffer[0] = '\0';
	DBGPrint("enter new value, <enter> to skip, or '.' to exit\n");
	while (changeBuffer[0] != '.')
	{
	   DBGPrint("[%08X] ", (unsigned)changeW);
	   for (r = 0; r < 4; r++)
	   {
	      oldW = (unsigned short) mdb_getword((unsigned long)changeW, 2);
	      DBGPrint("(%04X)=", (unsigned) oldW);

              ScreenInputFromKeyboard(&changeBuffer[0], 0, 6);

	      if ((changeBuffer[0] == '.') || (changeBuffer[1] == '.') ||
		  (changeBuffer[2] == '.') || (changeBuffer[3] == '.'))
		 break;
	      pB = (unsigned char *) &changeBuffer[0];
	      len = strlen(pB);

	      for (i = 0; i < len; i++)
		 DBGPrint("\b");

	      value = EvaluateExpression(0, &pB, &valid);
	      if (valid)
		 mdb_putword((unsigned long)changeW, value, 2);
	      DBGPrint("%04X ", (unsigned) mdb_getword((unsigned long)changeW, 2));
	      changeW++;
	   }
	   if (DBGPrint("\n")) return 1;
	}
	return 1;
     }
     DBGPrint("invalid change (word) address\n");
     return 1;
}

/* CD */

unsigned long changeDoubleValue(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     register unsigned char *changeBuffer = &workbuf[0][0];
     register unsigned long *changeD, oldD;
     register unsigned long address, r, value, len, i;
     unsigned char *pB;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	changeD = (unsigned long *) address;
	changeBuffer[0] = '\0';
	DBGPrint("enter new value, <enter> to skip, or '.' to exit\n");
	while (changeBuffer[0] != '.')
	{
	   DBGPrint("[%08X] ", (unsigned)changeD);
	   for (r = 0; r < 2; r++)
	   {
	      oldD = (unsigned long) mdb_getword((unsigned long)changeD, 4);
	      DBGPrint("(%08X)=", (unsigned) oldD);

	      ScreenInputFromKeyboard(&changeBuffer[0], 0, 8);

	      if ((changeBuffer[0] == '.') || (changeBuffer[1] == '.') ||
		  (changeBuffer[2] == '.') || (changeBuffer[3] == '.') ||
		  (changeBuffer[4] == '.') || (changeBuffer[5] == '.') ||
		  (changeBuffer[6] == '.') || (changeBuffer[7] == '.'))
		 break;

	      pB = (unsigned char *) &changeBuffer[0];
	      len = strlen(pB);

	      for (i = 0; i < len; i++)
		 DBGPrint("\b");

	      value = EvaluateExpression(0, &pB, &valid);
	      if (valid)
		 mdb_putword((unsigned long)changeD, value, 4);
	      DBGPrint("%08X ", (unsigned)mdb_getword((unsigned long)changeD, 4));
	      changeD++;
	   }
	   if (DBGPrint("\n")) return 1;
	}
	return 1;
     }
     DBGPrint("invalid change (dword) address\n");
     return 1;
}

/* CB */

unsigned long changeByteValue(unsigned char *cmd,
	       StackFrame *stackFrame, unsigned long Exception,
	       DEBUGGER_PARSER *parser)
{
     unsigned char *changeBuffer = &workbuf[0][0];
     register unsigned char *changeB, oldB;
     unsigned char *pB;
     register unsigned long address, r, value, len, i;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	changeB = (unsigned char *) address;
	changeBuffer[0] = '\0';
	DBGPrint("enter new value, <enter> to skip, or '.' to exit\n");
	while (changeBuffer[0] != '.')
	{
	   DBGPrint("[%08X] ", (unsigned)changeB);
	   for (r = 0; r < 8; r++)
	   {
	      oldB = (unsigned char) mdb_getword((unsigned long)changeB, 1);
	      DBGPrint("(%02X)=", (unsigned) oldB);

	      ScreenInputFromKeyboard(&changeBuffer[0], 0, 4);

	      if ((changeBuffer[0] == '.') || (changeBuffer[1] == '.'))
		 break;

	      pB = (unsigned char *) &changeBuffer[0];
	      len = strlen(pB);
	      for (i = 0; i < len; i++)
		 DBGPrint("\b");

	      value = EvaluateExpression(0, &pB, &valid);
	      if (valid)
		 mdb_putword((unsigned long)changeB, value, 1);
	      DBGPrint("%02X ", (unsigned char) mdb_getword((unsigned long)changeB, 1));
	      changeB++;
	   }
	   if (DBGPrint("\n")) return 1;
	}
	return 1;
     }
     DBGPrint("invalid change (byte) address\n");
     return 1;
}

/* C */

unsigned long changeDefaultValue(unsigned char *cmd,
			StackFrame *stackFrame, unsigned long Exception,
			DEBUGGER_PARSER *parser)
{
     unsigned char *changeBuffer = &workbuf[0][0];
     register unsigned char *changeB, oldB;
     unsigned char *pB;
     register unsigned long address, r, value, len, i;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     address = EvaluateExpression(stackFrame, &cmd, &valid);
     if (valid)
     {
	changeB = (unsigned char *) address;
	changeBuffer[0] = '\0';
	DBGPrint("enter new value, <enter> to skip, or '.' to exit\n");
	while (changeBuffer[0] != '.')
	{
	   DBGPrint("[%08X] ", (unsigned)changeB);
	   for (r = 0; r < 8; r++)
	   {
	      oldB = (unsigned char) mdb_getword((unsigned long)changeB, 1);
	      DBGPrint("(%02X)=", (unsigned char) oldB);

	      ScreenInputFromKeyboard(&changeBuffer[0], 0, 4);

	      if ((changeBuffer[0] == '.') || (changeBuffer[1] == '.'))
		 break;

	      pB = (unsigned char *) &changeBuffer[0];
	      len = strlen(pB);

	      for (i = 0; i < len; i++)
		 DBGPrint("\b");

	      value = EvaluateExpression(0, &pB, &valid);
	      if (valid)
		 mdb_putword((unsigned long)changeB, value, 1);
	      DBGPrint("%02X ", (unsigned char) mdb_getword((unsigned long)changeB, 1));
	      changeB++;
	   }
	   if (DBGPrint("\n")) return 1;
	}
	return 1;
     }
     DBGPrint("invalid change (byte) address\n");
     return 1;

}

unsigned long displayCloseHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("?   <address>            - display closest symbols to <address>\n");
    return 1;

}

/* ? */

unsigned long displayCloseSymbols(unsigned char *cmd,
			 StackFrame *stackFrame, unsigned long Exception,
			 DEBUGGER_PARSER *parser)
{
     register unsigned long oldD;
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     oldD = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
	oldD = GetIP(stackFrame);
     DisplayClosestSymbol(oldD);
     return 1;

}

unsigned long debuggerWalkStack(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastDumpAddress = dumpStack(stackFrame,
                          (unsigned char *)lastDumpAddress, lastDisplayLength);
	return 1;
     }
     lastDumpAddress = (unsigned char *) EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
	lastDumpAddress = (unsigned char *) GetStackAddress(stackFrame);
     displayLength = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
	displayLength = 20;
     lastDumpAddress = dumpStack(stackFrame, (unsigned char *)lastDumpAddress, displayLength);
     return 1;

}

unsigned long displayDumpHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("d   <address> <#lines>   - dump memory as bytes\n");
    DBGPrint("dw  <address> <#lines>   - dump memory as words\n");
    DBGPrint("dd  <address> <#lines>   - dump memory as double words\n");
    DBGPrint("dl  <address> <#lines>   - dump linked list\n");
    DBGPrint("ds  <address> <#lines>   - dump stack\n");
    DBGPrint("dds <address> <#lines>   - dump stack double word\n");
    DBGPrint("w   <address>            - display symbols on the stack\n");

    return 1;
}

/* DL */

unsigned long debuggerDumpLinkedList(unsigned char *cmd,
			    StackFrame *stackFrame, unsigned long Exception,
			    DEBUGGER_PARSER *parser)
{
     unsigned long valid;
     extern unsigned char *lastLinkAddress;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastLinkAddress = dumpLinkedList((unsigned char *)lastLinkAddress, lastDisplayLength, 0);
	return 1;
     }

     lastLinkAddress = (unsigned char *) EvaluateNumericExpression(stackFrame, &cmd,
                                                          &valid);
     if (!valid)
	lastLinkAddress = (unsigned char *) GetStackAddress(stackFrame);

     displayLength = EvaluateNumericExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
	displayLength = 20;

     lastLinkAddress = dumpLinkedList((unsigned char *)lastLinkAddress, displayLength, 0);

     return 1;

}

/* DW */

unsigned long debuggerDumpWord(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastDumpAddress = dumpWord((unsigned char *)lastDumpAddress, lastDisplayLength);
	return 1;
     }
     lastDumpAddress = (unsigned char *) EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
	lastDumpAddress = (unsigned char *) GetStackAddress(stackFrame);
     displayLength = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
	displayLength = 20;
     lastDumpAddress = dumpWord((unsigned char *)lastDumpAddress, displayLength);
     return 1;
}

/* DS */

unsigned long debuggerDumpStack(unsigned char *cmd,
		       StackFrame *stackFrame, unsigned long Exception,
		       DEBUGGER_PARSER *parser)
{
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastDumpAddress = dumpStack(stackFrame, (unsigned char *)lastDumpAddress, lastDisplayLength);
	return 1;
     }
     lastDumpAddress = (unsigned char *) EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
	lastDumpAddress = (unsigned char *) GetStackAddress(stackFrame);
     displayLength = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
	displayLength = 20;
     lastDumpAddress = dumpStack(stackFrame, (unsigned char *)lastDumpAddress, displayLength);
     return 1;

}

/* DDS */

unsigned long debuggerDumpDoubleStack(unsigned char *cmd,
			     StackFrame *stackFrame, unsigned long Exception,
			     DEBUGGER_PARSER *parser)
{
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastDumpAddress = dumpDoubleStack(stackFrame, (unsigned char *)lastDumpAddress,
						    lastDisplayLength);
	return 1;
     }
     lastDumpAddress = (unsigned char *) EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
	lastDumpAddress = (unsigned char *) GetStackAddress(stackFrame);
     displayLength = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
	displayLength = 20;
     lastDumpAddress = dumpDoubleStack(stackFrame, (unsigned char *)lastDumpAddress,
						    displayLength);
     return 1;

}

/* DD */

unsigned long debuggerDumpDouble(unsigned char *cmd,
			StackFrame *stackFrame, unsigned long Exception,
			DEBUGGER_PARSER *parser)
{
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastDumpAddress = dumpDouble((unsigned char *)lastDumpAddress, lastDisplayLength);
	return 1;
     }
     lastDumpAddress = (unsigned char *) EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
	lastDumpAddress = (unsigned char *) GetStackAddress(stackFrame);
     displayLength = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
	displayLength = 20;
     lastDumpAddress = dumpDouble((unsigned char *)lastDumpAddress, displayLength);
     return 1;

}

/* D */

unsigned long debuggerDumpByte(unsigned char *cmd,
		      StackFrame *stackFrame, unsigned long Exception,
		      DEBUGGER_PARSER *parser)
{
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastDumpAddress = dump((unsigned char *)lastDumpAddress, lastDisplayLength);
	return 1;
     }
     lastDumpAddress = (unsigned char *) EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
	lastDumpAddress = (unsigned char *) GetStackAddress(stackFrame);
     displayLength = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
	displayLength = 20;
     lastDumpAddress = dump((unsigned char *)lastDumpAddress, displayLength);
     return 1;

}

unsigned long displayDisassembleHelp(unsigned char *commandLine, DEBUGGER_PARSER *parser)
{
    DBGPrint("id  <address> <#lines>   - unassemble code (32-bit)\n");
    DBGPrint("u   <address> <#lines>   - unassemble code (32-bit)\n");
    DBGPrint("uu  <address> <#lines>   - unassemble code (16-bit)\n");
    return 1;
}

/* UU */

unsigned long processDisassemble16(unsigned char *cmd,
			  StackFrame *stackFrame, unsigned long Exception,
			  DEBUGGER_PARSER *parser)
{
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastUnasmAddress = disassemble(stackFrame, (unsigned long)lastUnasmAddress,
						    lastDisplayLength, 0);
	return 1;
     }
     lastUnasmAddress = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
     {
        if (!*cmd)
	   lastUnasmAddress = GetIP(stackFrame);
        else
        {
           DBGPrint("invalid address for unassemble\n");
           return 1;
        }
     }
     displayLength = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
		     displayLength = 20;
     lastUnasmAddress = disassemble(stackFrame, (unsigned long)lastUnasmAddress,
						 displayLength, 0);
     return 1;
}

/* U */

unsigned long processDisassemble32(unsigned char *cmd,
			  StackFrame *stackFrame, unsigned long Exception,
			  DEBUGGER_PARSER *parser)
{
     unsigned long valid;

     cmd = &cmd[parser->debugCommandNameLength];
     while (*cmd && *cmd == ' ')
        cmd++;

     if (repeatCommand)
     {
	lastUnasmAddress = disassemble(stackFrame, (unsigned long)lastUnasmAddress,
				       lastDisplayLength, 1);
	return 1;
     }
     lastUnasmAddress = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!valid)
     {
        if (!*cmd)
	   lastUnasmAddress = GetIP(stackFrame);
        else
        {
           DBGPrint("invalid address for unassemble\n");
           return 1;
        }
     }
     displayLength = EvaluateExpression(stackFrame, &cmd, &valid);
     if (!displayLength || displayLength > 20)
	displayLength = 20;
     lastUnasmAddress = disassemble(stackFrame, (unsigned long)lastUnasmAddress,
						 displayLength, 1);
     return 1;

}
