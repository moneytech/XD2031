/* 
 * XD2031 - Serial line file server for CBMs
 * Copyright (C) 2012 Andre Fachat <afachat@gmx.de>
 * 
 * Taken over / derived from:
 * sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007-2012  Ingo Korb <ingo@akana.de>
   Inspired by MMC2IEC by Lars Pontoppidan et al.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


   errormsg.h: Definitions for the error message generator

*/

/**
 * Note that this error message code is deficient for a device that 
 * has both IEEE and IEC busses, as it only supports a single error 
 * state.
 */
#ifndef ERRORMSG_H
#define ERRORMSG_H

#include <stdint.h>

#include "config.h"


//typedef void errormsg_t;

typedef struct {
	// callback that is called when the buffer is to be reset
	void 		(*set_ok_message)();	// (struct errormsg_t *err);
	// error buffer
	uint8_t 	error_buffer[CONFIG_ERROR_BUFFER_SIZE];
} errormsg_t ;

void set_error_ts(errormsg_t *error, uint8_t errornum, uint8_t track, uint8_t sector);

static inline void set_error(errormsg_t *error, uint8_t errornum) {
	set_error_ts(error, errornum, 0, 0);
}

// Commodore DOS error codes
#define ERROR_OK                  0
#define ERROR_SCRATCHED           1
#define ERROR_PARTITION_SELECTED  2
#define ERROR_STATUS              3
#define ERROR_LONGVERSION         9
#define ERROR_READ_NOHEADER      20
#define ERROR_READ_NOSYNC        21
#define ERROR_READ_NODATA        22
#define ERROR_READ_CHECKSUM      23
#define ERROR_WRITE_VERIFY       25
#define ERROR_WRITE_PROTECT      26
#define ERROR_READ_HDRCHECKSUM   27
#define ERROR_DISK_ID_MISMATCH   29
#define ERROR_SYNTAX_UNKNOWN     30
#define ERROR_SYNTAX_UNABLE      31
#define ERROR_SYNTAX_TOOLONG     32
#define ERROR_SYNTAX_JOKER       33
#define ERROR_SYNTAX_NONAME      34
#define ERROR_FILE_NOT_FOUND_39  39
#define ERROR_RECORD_MISSING     50
#define ERROR_RECORD_OVERFLOW    51
#define ERROR_FILE_TOO_LARGE     52
#define ERROR_WRITE_FILE_OPEN    60
#define ERROR_FILE_NOT_OPEN      61
#define ERROR_FILE_NOT_FOUND     62
#define ERROR_FILE_EXISTS        63
#define ERROR_FILE_TYPE_MISMATCH 64
#define ERROR_NO_BLOCK           65
#define ERROR_ILLEGAL_TS_COMMAND 66
#define ERROR_ILLEGAL_TS_LINK    67
#define ERROR_NO_CHANNEL         70
#define ERROR_DIR_ERROR          71
#define ERROR_DISK_FULL          72
#define ERROR_DOSVERSION         73
#define ERROR_DRIVE_NOT_READY    74
#define ERROR_PARTITION_ILLEGAL  77
#define ERROR_BUFFER_TOO_SMALL   78
#define ERROR_IMAGE_INVALID      79
#define ERROR_CLOCK_UNSTABLE     99

/// Version number string, will be added to message 73
//extern const char PROGMEM versionstr[];

/// Long version string, used for message 9
//extern const char PROGMEM longverstr[];

#endif
