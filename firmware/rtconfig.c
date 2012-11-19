
/****************************************************************************

    XD-2031 - Serial line filesystem server for CBMs
    Copyright (C) 2012 Andre Fachat

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
    MA  02110-1301, USA.

****************************************************************************/

/**
 * This file implements the central communication channels between the
 * IEEE layer and the provider layers
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "rtconfig.h"
#include "errors.h"
#include "provider.h"	// MAX_DRIVES
#include "nvconfig.h"
#include "bus.h"	// get_default_device_address()
#include "system.h"	// reset_mcu()

#include "debug.h"

#define	MAX_RTCONFIG	3

static endpoint_t *endpoint;

static rtconfig_t *rtcs[MAX_RTCONFIG];

static int num_rtcs = 0;

void rtconfig_init(endpoint_t *_endpoint) {
	num_rtcs = 0;
	endpoint = _endpoint;
}

// initialize a runtime config block
void rtconfig_init_rtc(rtconfig_t *rtc, uint8_t devaddr) {
	// Default values
	rtc->device_address = devaddr;
	rtc->last_used_drive = 0;

	if(nv_restore_config(rtc)) nv_save_config(rtc);

	if (num_rtcs < MAX_RTCONFIG) {
		rtcs[num_rtcs] = rtc;
		num_rtcs++;
	} else {
		term_printf("too many rtconfigs!\n");
	}
}

/********************************************************************************/

#define	OPT_BUFFER_LENGTH	64

static char buf[OPT_BUFFER_LENGTH];

static packet_t buspack;

// there shouldn't be much debug output, as sending it may invariably 
// receive the next option, triggering the option again. But it isn't
// re-entrant!
static uint8_t setopt_callback(int8_t channelno, int8_t errno) {

        //debug_printf("setopt cb err=%d\n", errno);
        if (errno == ERROR_OK) {
                //debug_printf("rx command: %s\n", buf);

		uint8_t len = buf[FSP_LEN];

		// find the correct rtconfig
		for (uint8_t i = 0; i < num_rtcs; i++) {
			rtconfig_t *rtc = rtcs[i];
			const char *name = rtc->name;

			uint8_t j;
			for (j = 0; j < len; j++) {
				if (name[j] == 0
					|| name[j] != buf[j]) {
					break;
				}
			}
			if ( buf[j] == ':') {
				// found it, now set the config
				buf[j] = 'X';
				rtconfig_set(rtc, buf+j);
				break;
			}
		}
        }
        return 1;
}


void rtconfig_pullconfig() {
        // init the packet struct
        packet_init(&buspack, OPT_BUFFER_LENGTH, (uint8_t*)buf);

        // prepare FS_RESET packet
        packet_set_filled(&buspack, FSFD_SETOPT, FS_RESET, 0);

        // send request, receive in same buffer we sent from
        endpoint->provider->submit_call(endpoint->provdata, FSFD_SETOPT, &buspack, &buspack, setopt_callback);

        debug_printf("sent reset packet on fd %d\n", FSFD_SETOPT);
}

/********************************************************************************/

// set from an X command
errno_t rtconfig_set(rtconfig_t *rtc, const char *cmd) {

	debug_printf("CMD:'%s'\n", cmd);

	errno_t er = ERROR_SYNTAX_UNKNOWN;

	const char *ptr = cmd;

	char c = *ptr;
	while (c != 0 && c !='X') {
		ptr++;
		c = *ptr;
	}

	do {
		ptr++;
		c = *ptr;
	} while (c == ' ');

	// c now contains the actual command
	switch(c) {
	case 'U':
		// look for "U=<unit number in ascii || binary>"
		ptr++;
		if (*ptr == '=') {
			ptr++;
			uint8_t devaddr = (*ptr);
			if (isdigit(*ptr)) devaddr = atoi(ptr);
			if (devaddr >= 4 && devaddr <= 30) {
				rtc->device_address = devaddr;
				er = ERROR_OK;
				debug_printf("SETTING UNIT# TO %d\n", devaddr);
			} else {
				er = ERROR_SYNTAX_INVAL;
				debug_printf("ERROR SETTING UNIT# TO %d\n", devaddr);
			}
		}
		break;
	case 'D':
		// set default drive number
		// look for "D=<drive number in ascii || binary>"
		ptr++;
		if (*ptr == '=') {
			ptr++;
			uint8_t drv = (*ptr);
			if (isdigit(*ptr)) drv=atoi(ptr);
			if (drv < MAX_DRIVES) {
				rtc->last_used_drive = drv;
				er = ERROR_OK;
				debug_printf("SETTING DRIVE# TO %d\n", drv);
			}
		}
		break;
	case 'I':
		// INIT: restore default values
		rtconfig_init_rtc(rtc, get_default_device_address());
		er = ERROR_OK;
		debug_puts("RUNTIME CONFIG INITIALIZED\n");
	case 'W':
		// write runtime config to EEPROM
		nv_save_config(rtc);
		er = ERROR_OK;
		break;
	case 'R':
		if(!strcmp(ptr, "RESET")) {
			// reset everything
			reset_mcu();
		}
	default:
		break;
	}

	return er;
}


