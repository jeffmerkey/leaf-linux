
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

#ifndef _MDB_LIST
#define _MDB_LIST

typedef struct _ACCELERATOR
{
    struct _ACCELERATOR *accelNext;
    struct _ACCELERATOR *accelPrior;
    unsigned long (*accelRoutine)(unsigned long key, void *p, struct _ACCELERATOR *parser);
    unsigned long (*accelRoutineHelp)(unsigned long key, struct _ACCELERATOR *parser);
    unsigned long accelFlags;
    unsigned long key;
    unsigned long supervisorCommand;
    unsigned char *shortHelp;
} ACCELERATOR;

typedef struct _ALT_DEBUGGER
{
    struct _ALT_DEBUGGER *altDebugNext;
    struct _ALT_DEBUGGER *altDebugPrior;
    int (*AlternateDebugger)(int reason, int error, void *frame);
} ALT_DEBUGGER;

extern int AlternateDebuggerRoutine(int reason, int error, void *frame);
extern unsigned long AddAlternateDebugger(ALT_DEBUGGER *Debugger);
extern unsigned long RemoveAlternateDebugger(ALT_DEBUGGER *Debugger);

typedef struct _DEBUGGER_PARSER {
    struct _DEBUGGER_PARSER *debugNext;
    struct _DEBUGGER_PARSER *debugPrior;
    unsigned long (*DebugCommandParser)(unsigned char *commandLine,
			       StackFrame *stackFrame, unsigned long Exception,
			       struct _DEBUGGER_PARSER *parser);
    unsigned long (*DebugCommandParserHelp)(unsigned char *commandLine,
				   struct _DEBUGGER_PARSER *parser);
    unsigned long parserFlags;
    unsigned char *debugCommandName;
    unsigned long debugCommandNameLength;
    unsigned long supervisorCommand;
    unsigned char *shortHelp;
    unsigned long controlTransfer;
} DEBUGGER_PARSER;

typedef struct _DEBUGGER_LIST
{
   DEBUGGER_PARSER *head;
   DEBUGGER_PARSER *tail;
} DEBUGGER_LIST;

extern unsigned long DebuggerParserRoutine(unsigned char *command, unsigned char *commandLine,
			   StackFrame *stackFrame, unsigned long Exception);
extern unsigned long DebuggerParserHelpRoutine(unsigned char *command, unsigned char *commandLine);
extern unsigned long AddDebuggerCommandParser(DEBUGGER_PARSER *parser);
extern unsigned long RemoveDebuggerCommandParser(DEBUGGER_PARSER *parser);

static inline unsigned long strhash(unsigned char *s, int len, int limit)
{
   register unsigned long h = 0, a = 127, i;

   if (!limit)
      return -1;

   for (i = 0; i < len && *s; s++, i++)
      h = ((a * h) + *s) % limit;

   return h;
}

#endif
