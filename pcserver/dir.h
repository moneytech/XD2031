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

#ifndef DIR_H
#define	DIR_H

/**
 * fopen the first matching directory entry, using the given
 * options string
 */
FILE *open_first_match(const char *dir, const char *pattern, const char *options);

/**
 * calls the callback on every matching file, returning the number of matches
 * The callback gets the match count as first parameter (starting with one),
 * and if it returns != 0 then the loop is exited.
 */
int dir_call_matches(const char *dir, const char *pattern, int (*callback)(const int num_of_match, const char *name));

/**
 * fill the buffer with a header entry, using the driveno as line number
 * and dirpattern as file name
 *
 * returns the length of the written buffer
 */
int dir_fill_header(char *dest, int driveno, char *dirpattern);

/**
 * finds the next directory entry matching the given directory pattern
 */
struct dirent* dir_next(DIR *dp, char *dirpattern);

/**
 * fill in the buffer with a directory entry
 *
 * returns the length of the written buffer
 */
int dir_fill_entry(char *dest, struct dirent *de, int maxsize);

/**
 * fill in the buffer with the final disk info entry
 *
 * returns the length of the written buffer
 */
int dir_fill_disk(char *dest);

/**
 * malloc a new path, and copy the given base path and name to it,
 * with a separating char
 */
char *malloc_path(const char *base, const char *name);

#endif

