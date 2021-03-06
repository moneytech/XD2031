/****************************************************************************

    Serial line filesystem server
    Copyright (C) 2012 Andre Fachat

    Derived from:
    OS/A65 Version 1.3.12
    Multitasking Operating System for 6502 Computers
    Copyright (C) 1989-1997 Andre Fachat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

****************************************************************************/

void set_verbose(int flag);

void log_errno(const char *msg, ...);

void log_warn(const char *msg, ...);

void log_error(const char *msg, ...);

void log_info(const char *msg, ...);

void log_debug(const char *msg, ...);

void log_term(const char *msg);

void log_hexdump(const char *p, int len, int petscii);

void log_hexdump2(const char *p, int len, int petscii, const char *prefix);

#define	log_entry(func)	log_debug("ENTER: %s\n", (func))
#define	log_exit()	log_debug("EXIT")
#define	log_exitr(rv)	log_debug("EXIT: rv=%d\n", (rv))
#define	log_rv(rv)	log_error("ERROR RETURN: %d\n", (rv))

const char *dump_indent(int n);
