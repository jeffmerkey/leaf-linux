
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
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/atomic.h>
#include <asm/msr.h>
#include <linux/io.h>

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

#ifdef MDB_ATOMIC
static spinlock_t accelLock = SPIN_LOCK_UNLOCKED;
static long accelflags;
#endif

ACCELERATOR *accelHead;
ACCELERATOR *accelTail;

unsigned long AccelRoutine(unsigned long key, void *p)
{
    register ACCELERATOR *accel;
    unsigned long retCode;

    accel = accelHead;
    while (accel)
    {
       if (accel->accelFlags && accel->accelRoutine && accel->key == key)
       {
	  retCode = (accel->accelRoutine)(key, p, accel);
	  return retCode;
       }
       accel = accel->accelNext;
    }
    return 0;
}

void ShowDebuggerAccelerators(void)
{
   register ACCELERATOR *accel;

   DBGPrint("\nDebugger Keystroke Accelerator(s)\n");

   accel = accelHead;
   while (accel)
   {
      if (accel->shortHelp)
         if (DBGPrint("%s\n", accel->shortHelp)) return;
      accel = accel->accelNext;
   }
   return;
}

unsigned long IsAccelerator(unsigned long key)
{
    register ACCELERATOR *accel;

    accel = accelHead;
    while (accel)
    {
       if (accel->accelFlags && accel->accelRoutine && accel->key == key)
       {
	  return 1;
       }
       accel = accel->accelNext;
    }
    return 0;
}

unsigned long AccelHelpRoutine(unsigned long key)
{
    register ACCELERATOR *accel;

    accel = accelHead;
    if (key)  /* if we were passed a command string */
    {
       while (accel)
       {
	  if (accel->accelFlags && accel->key == key)
	  {
	     if (accel->accelRoutineHelp)
	     {
		DBGPrint("Accelerator %08X\n", (unsigned)accel->key);
		(accel->accelRoutineHelp)(key, accel);
		return 1;
	     }
	     DBGPrint("Accelerator %08X\n", (unsigned)accel->key);
	     return 1;
	  }
	  accel = accel->accelNext;
       }
       DBGPrint("Help for Accelerator [%08X] not found\n", (unsigned)key);
       return 1;
    }
    else
    {
       DBGPrint("Accelerator(s)\n");
       while (accel)
       {
	  if (accel->accelFlags && accel->key && !accel->supervisorCommand)
	     DBGPrint("%08X         - %s\n",
                      (unsigned)accel->key, accel->shortHelp);
	  accel = accel->accelNext;
       }
    }
    return 0;
}

ACCELERATOR *insertAccel(ACCELERATOR *i, ACCELERATOR *top)
{
    ACCELERATOR *old, *p;

    if (!accelTail)
    {
       i->accelNext = i->accelPrior = NULL;
       accelTail = i;
       return i;
    }
    p = top;
    old = NULL;
    while (p)
    {
       if (p->key < i->key)
       {
	  old = p;
	  p = p->accelNext;
       }
       else
       {
	  if (p->accelPrior)
	  {
	     p->accelPrior->accelNext = i;
	     i->accelNext = p;
	     i->accelPrior = p->accelPrior;
	     p->accelPrior = i;
	     return top;
	  }
	  i->accelNext = p;
	  i->accelPrior = NULL;
	  p->accelPrior = i;
	  return i;
       }
    }
    old->accelNext = i;
    i->accelNext = NULL;
    i->accelPrior = old;
    accelTail = i;
    return accelHead;

}

unsigned long AddAccelRoutine(ACCELERATOR *newAccel)
{
    register ACCELERATOR *accel;

#ifdef MDB_ATOMIC
    spin_lock_irqsave(&accelLock, accelflags);
#endif
    accel = accelHead;
    while (accel)
    {
       if (accel == newAccel || accel->key == newAccel->key)
       {
#ifdef MDB_ATOMIC
	  spin_unlock_irqrestore(&accelLock, accelflags);
#endif
	  return 1;
       }
       accel = accel->accelNext;
    }
    newAccel->accelFlags = -1;
    accelHead = insertAccel(newAccel, accelHead);

#ifdef MDB_ATOMIC
    spin_unlock_irqrestore(&accelLock, accelflags);
#endif
    return 0;
}

unsigned long RemoveAccelRoutine(ACCELERATOR *newAccel)
{
    register ACCELERATOR *accel;

#ifdef MDB_ATOMIC
    spin_lock_irqsave(&accelLock, accelflags);
#endif
    accel = accelHead;
    while (accel)
    {
       if (accel == newAccel)   /* found, remove from list */
       {
	  if (accelHead == newAccel)
	  {
	     accelHead = (void *) newAccel->accelNext;
	     if (accelHead)
		accelHead->accelPrior = NULL;
	     else
		accelTail = NULL;
	  }
	  else
	  {
	     newAccel->accelPrior->accelNext = newAccel->accelNext;
	     if (newAccel != accelTail)
		newAccel->accelNext->accelPrior = newAccel->accelPrior;
	     else
		accelTail = newAccel->accelPrior;
	  }
	  newAccel->accelNext = newAccel->accelPrior = 0;
	  newAccel->accelFlags = 0;

#ifdef MDB_ATOMIC
	  spin_unlock_irqrestore(&accelLock, accelflags);
#endif
	  return 0;
       }
       accel = accel->accelNext;
    }

#ifdef MDB_ATOMIC
    spin_unlock_irqrestore(&accelLock, accelflags);
#endif
    return -1;
}

#ifdef MDB_ATOMIC
static spinlock_t altDebugLock = SPIN_LOCK_UNLOCKED;
static long altflags;
#endif

ALT_DEBUGGER *altDebugHead;
ALT_DEBUGGER *altDebugTail;

int AlternateDebuggerRoutine(int reason, int error, void *frame)
{
    register ALT_DEBUGGER *altDebug;
    register unsigned long retCode;
    unsigned long state;

    state = save_flags();
    altDebug = altDebugHead;
    while (altDebug)
    {
       if (altDebug->AlternateDebugger)
       {
	  retCode = (altDebug->AlternateDebugger)(reason, error, frame);
	  if (retCode)
          {
             restore_flags(state);
	     return retCode;
          }
       }
       altDebug = altDebug->altDebugNext;
    }
    restore_flags(state);
    return 0;
}

unsigned long AddAlternateDebugger(ALT_DEBUGGER *Debugger)
{
    register ALT_DEBUGGER *altDebug;

#ifdef MDB_ATOMIC
    spin_lock_irqsave(&altDebugLock, altflags);
#endif
    altDebug = altDebugHead;
    while (altDebug)
    {
       if (altDebug == Debugger)
       {
#ifdef MDB_ATOMIC
	  spin_unlock_irqrestore(&altDebugLock, altflags);
#endif
	  return 1;
       }
       altDebug = altDebug->altDebugNext;
    }
    if (!altDebugHead)
    {
       altDebugHead = Debugger;
       altDebugTail = Debugger;
       Debugger->altDebugNext = 0;
       Debugger->altDebugPrior = 0;
    }
    else
    {
       altDebugTail->altDebugNext = Debugger;
       Debugger->altDebugNext = 0;
       Debugger->altDebugPrior = altDebugTail;
       altDebugTail = Debugger;
    }

#ifdef MDB_ATOMIC
    spin_unlock_irqrestore(&altDebugLock, altflags);
#endif
    return 0;
}

unsigned long RemoveAlternateDebugger(ALT_DEBUGGER *Debugger)
{
    register ALT_DEBUGGER *altDebug;

#ifdef MDB_ATOMIC
    spin_lock_irqsave(&altDebugLock, altflags);
#endif
    altDebug = altDebugHead;
    while (altDebug)
    {
       if (altDebug == Debugger)   /* found, remove from list */
       {
	  if (altDebugHead == Debugger)
	  {
	     altDebugHead = (void *) Debugger->altDebugNext;
	     if (altDebugHead)
		altDebugHead->altDebugPrior = NULL;
	     else
		altDebugTail = NULL;
	  }
	  else
	  {
	     Debugger->altDebugPrior->altDebugNext = Debugger->altDebugNext;
	     if (Debugger != altDebugTail)
		Debugger->altDebugNext->altDebugPrior = Debugger->altDebugPrior;
	     else
		altDebugTail = Debugger->altDebugPrior;
	  }
	  Debugger->altDebugNext = Debugger->altDebugPrior = 0;

#ifdef MDB_ATOMIC
          spin_unlock_irqrestore(&altDebugLock, altflags);
#endif
	  return 0;
       }
       altDebug = altDebug->altDebugNext;
    }
#ifdef MDB_ATOMIC
    spin_unlock_irqrestore(&altDebugLock, altflags);
#endif
    return -1;
}

#ifdef MDB_ATOMIC
static spinlock_t debugParserLock = SPIN_LOCK_UNLOCKED;
static long parserflags;
#endif

DEBUGGER_PARSER *debugParserHead;
DEBUGGER_PARSER *debugParserTail;

unsigned long DebuggerParserRoutine(unsigned char *command, unsigned char *commandLine,
			    StackFrame *stackFrame, unsigned long Exception)
{
    register DEBUGGER_PARSER *debugParser;
    register unsigned long retCode, valid = 0, length;
    register unsigned char *p;

    p = commandLine;
    if (!p)
       return 0;

    /* if a passed string is just whitespace, return error */
    while (*p)
    {
       if ((*p != ' ') && (*p != '\n') && (*p != '\r'))
       {
          valid = 1;
          break;
       }
       p++;
    }
    if (!valid)
       return 0;

    UpcaseString(command);
    length = strlen(command);

    debugParser = debugParserHead;
    while (debugParser)
    {
       if (debugParser->parserFlags && debugParser->DebugCommandParser &&
	   (debugParser->debugCommandNameLength == length) &&
	   (!strcmp(debugParser->debugCommandName, command)))
       {
	  retCode = (debugParser->DebugCommandParser)(commandLine, stackFrame,
						      Exception, debugParser);
	  if (retCode)
	     return retCode;
       }
       debugParser = debugParser->debugNext;
    }

    DBGPrint("unknown mdb command -> %s\n", command);
    return 0;
}

unsigned long DebuggerParserHelpRoutine(unsigned char *command, unsigned char *commandLine)
{
    extern void ShowDebuggerAccelerators(void);
    register DEBUGGER_PARSER *debugParser;
    register unsigned long length;

    UpcaseString(command);
    length = strlen(command);
    if (*command)  /* if we were passed a command string */
    {

       debugParser = debugParserHead;
       while (debugParser)
       {
	  if (debugParser->parserFlags &&
             (debugParser->debugCommandNameLength == length) &&
             !strcmp(debugParser->debugCommandName, command))
	  {
	     if (debugParser->DebugCommandParserHelp)
	     {
		DBGPrint("Help for Command %s\n",
                         debugParser->debugCommandName);
		(debugParser->DebugCommandParserHelp)(commandLine, debugParser);
		return 1;
	     }
	     DBGPrint("Help for Command %s\n", debugParser->debugCommandName);
	     return 1;
	  }
	  debugParser = debugParser->debugNext;
       }

       DBGPrint("Help for Command [%s] not found\n", command);
       return 1;
    }
    else
    {
       DBGPrint("Debugger Command(s)\n");

       debugParser = debugParserHead;
       while (debugParser)
       {
	  if (debugParser->parserFlags && debugParser->debugCommandName &&
	      !debugParser->supervisorCommand)
	     if (DBGPrint("%15s    - %s\n", debugParser->debugCommandName,
                      debugParser->shortHelp)) return 0;
	  debugParser = debugParser->debugNext;
       }
       ShowDebuggerAccelerators();
    }
    return 0;
}

DEBUGGER_PARSER *insertDebuggerParser(DEBUGGER_PARSER *i, DEBUGGER_PARSER *top)
{
    DEBUGGER_PARSER *old, *p;

    if (!debugParserTail)
    {
       i->debugNext = i->debugPrior = NULL;
       debugParserTail = i;
       return i;
    }
    p = top;
    old = NULL;
    while (p)
    {
       if (strcmp(p->debugCommandName, i->debugCommandName) < 0)
       {
	  old = p;
	  p = p->debugNext;
       }
       else
       {
	  if (p->debugPrior)
	  {
	     p->debugPrior->debugNext = i;
	     i->debugNext = p;
	     i->debugPrior = p->debugPrior;
	     p->debugPrior = i;
	     return top;
	  }
	  i->debugNext = p;
	  i->debugPrior = NULL;
	  p->debugPrior = i;
	  return i;
       }
    }
    old->debugNext = i;
    i->debugNext = NULL;
    i->debugPrior = old;
    debugParserTail = i;
    return debugParserHead;

}

unsigned long AddDebuggerCommandParser(DEBUGGER_PARSER *parser)
{
    register DEBUGGER_PARSER *debugParser;

#ifdef MDB_ATOMIC
    spin_lock_irqsave(&debugParserLock, parserflags);
#endif
    parser->parserFlags = -1;
    parser->debugCommandNameLength = strlen(parser->debugCommandName);

    debugParser = debugParserHead;
    while (debugParser)
    {
       if (debugParser == parser ||
	  (parser->debugCommandNameLength == 
           debugParser->debugCommandNameLength &&
	  (!strcmp(parser->debugCommandName, debugParser->debugCommandName))))
       {
#ifdef MDB_ATOMIC
	  spin_unlock_irqrestore(&debugParserLock, parserflags);
#endif
	  return 1;
       }
       debugParser = debugParser->debugNext;
    }

    debugParserHead = insertDebuggerParser(parser, debugParserHead);

#ifdef MDB_ATOMIC
    spin_unlock_irqrestore(&debugParserLock, parserflags);
#endif
    return 0;
}

unsigned long RemoveDebuggerCommandParser(DEBUGGER_PARSER *parser)
{
    register DEBUGGER_PARSER *debugParser;

#ifdef MDB_ATOMIC
    spin_lock_irqsave(&debugParserLock, parserflags);
#endif
    debugParser = debugParserHead;
    while (debugParser)
    {
       if (debugParser == parser)   /* found, remove from list */
       {
	  if (debugParserHead == parser)
	  {
	     debugParserHead = (void *) parser->debugNext;
	     if (debugParserHead)
		debugParserHead->debugPrior = NULL;
	     else
		debugParserTail = NULL;
	  }
	  else
	  {
	     parser->debugPrior->debugNext = parser->debugNext;
	     if (parser != debugParserTail)
		parser->debugNext->debugPrior = parser->debugPrior;
	     else
		debugParserTail = parser->debugPrior;
	  }
	  parser->debugNext = parser->debugPrior = 0;
	  parser->parserFlags = 0;

#ifdef MDB_ATOMIC
	  spin_unlock_irqrestore(&debugParserLock, parserflags);
#endif
	  return 0;
       }
       debugParser = debugParser->debugNext;
    }
#ifdef MDB_ATOMIC
    spin_unlock_irqrestore(&debugParserLock, parserflags);
#endif
    return -1;
}

