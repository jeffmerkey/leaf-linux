
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

#ifndef _MDB_H
#define _MDB_H

/* screen output function */
extern int mdb_printf(char *s, ...);
#define DBGPrint   mdb_printf

/* external entry points used by linux */
#define DEBUGGER_EXCEPTION        1
#define NMI_EXCEPTION             2
#define BREAKPOINT_EXCEPTION      3
#define GENERAL_PROTECTION       13
#define PAGE_FAULT_EXCEPTION     14
#define KEYBOARD_ENTRY           19
#define SOFTWARE_EXCEPTION       22

/* internal trace messages */
#define MDB_DEBUG_DEBUGGER       0

#endif
