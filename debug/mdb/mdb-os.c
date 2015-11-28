
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
#include <linux/nmi.h>
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

/* module symbol workspace */
unsigned char symbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];
unsigned char modbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];
unsigned char workbuf[MAX_PROCESSORS][MAX_SYMBOL_LEN];
unsigned char traceBuf[MAX_SYMBOL_LEN];

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
          if (mdb_verify_rw((void *)&task->thread.esp, 4))
             return 0;

          stack = (unsigned long *)
                  mdb_getword((unsigned long)&task->thread.esp, 4);
       }
    }

#ifdef CONFIG_FRAME_POINTER
    if (!ebp)
    {
       if (task == current)
          asm ("movl %%ebp, %0" : "=r" (ebp) : );
       else
       {
          if (mdb_verify_rw((void *)task->thread.esp, 4))
             return 0;

          ebp = (unsigned long)mdb_getword(task->thread.esp, 4);
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

unsigned char *mdbprompt = "--- More (Q to Quit) ---";
int nextline;
int pause_mode = 1;
spinlock_t mdb_lock = SPIN_LOCK_UNLOCKED;
unsigned long mdb_flags;

int mdb_printf(char *fmt, ...)
{
	char buffer[256];
	char keystroke[16];
	va_list	ap;
	int linecount;
	struct console *con;

        linecount = 23;
	va_start(ap, fmt);
	vsprintf(buffer, fmt, ap);
	va_end(ap);

	for (con = console_drivers; con; con = con->next)
        {
	   if ((con->flags & CON_ENABLED) && con->write &&
	        (cpu_online(get_processor_id()) ||
		(con->flags & CON_ANYTIME)))
	      con->write(con, buffer, strlen(buffer));
	}

	if (strchr(buffer, '\n') != NULL)
	   nextline++;

	if (pause_mode && (nextline == linecount))
        {
	   nextline = 0;

	   for (con = console_drivers; con; con = con->next)
           {
	      if ((con->flags & CON_ENABLED) && con->write &&
	           (cpu_online(get_processor_id()) ||
	           (con->flags & CON_ANYTIME)))
	         con->write(con, mdbprompt, strlen(mdbprompt));
	   }

	   keystroke[0] = (char)mdb_getkey();
	   nextline = 1;
	   if ((keystroke[0] == 'q') || (keystroke[0] == 'Q'))
           {
	      for (con = console_drivers; con; con = con->next)
              {
		 if ((con->flags & CON_ENABLED) && con->write &&
		     (cpu_online(get_processor_id()) ||
		     (con->flags & CON_ANYTIME)))
		    con->write(con, "\n", 1);
	      }
              return 1;
           }
	}
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

int mdb_copy(void *to, void *from, size_t size)
{
    return __copy_to_user_inatomic(to, from, size);
}

int mdb_verify_rw(void *addr, size_t size)
{
    unsigned char data[size];
    return (mdb_copy(data, addr, size));
}

int mdb_getlword(unsigned long *word, unsigned long addr, size_t size)
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
		if (size <= sizeof(*word))
                {
			if (!(err = mdb_copy(&w8, (void *)addr, size)))
				*word = w8;
			break;
		}
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
		if (size <= sizeof(word))
                {
		   w8 = word;
		   err = mdb_copy((void *)addr, &w8, size);
		   break;
		}
	default:
		err = -EFAULT;
	}
	return (err);
}

unsigned long mdb_getword(unsigned long addr, size_t size)
{
   unsigned long data = 0;
   register int ret;

   ret = mdb_getlword(&data, addr, size);
   if (ret)
      return 0;

   return data;
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
    return;
}

unsigned short ReadFS(void)
{
    unsigned short contents = 0;

    __asm__ ("mov %%fs,%0\n\t":"=r"(contents));
    return contents;
}

unsigned short ReadGS(void)
{
    unsigned short contents = 0;

    __asm__ ("mov %%gs,%0\n\t":"=r"(contents));
    return contents;
}

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

unsigned long ReadDR0(void)  {  return (ReadDR(0)); }
unsigned long ReadDR1(void)  {  return (ReadDR(1)); }
unsigned long ReadDR2(void)  {  return (ReadDR(2)); }
unsigned long ReadDR3(void)  {  return (ReadDR(3)); }
unsigned long ReadDR6(void)  {  return (ReadDR(6)); }
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
