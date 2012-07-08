/****************************************************************************

    XD-2031 - Serial line filesystem server for CBMs
    Copyright (C) 2012 Andre Fachat

    Parts of this file derived from ieee.c in
    sd2iec - SD/MMC to Commodore serial bus interface/controller
    Copyright (C) 2007-2012  Ingo Korb <ingo@akana.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; 
    version 2 of the License ONLY.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

****************************************************************************/

/*
 * IEEE488 impedance layer
 *
 * provides the bus_sendbyte, bus_receivebyte and bus_attention
 * methods called by the IEEE loop, and translates these into 
 * calls to the channel framework, open calls etc.
 *
 * Uses a struct with state info, so can be called from both
 * IEEE488 as well as IEC bus
 */

#include "config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "channel.h"
#include "status.h"
#include "errormsg.h"
#include "cmd.h"
#include "file.h"
#include "bus.h"

#include "led.h"
#include "debug.h"

#include "xs1541.h"

#undef	DEBUG_SERIAL
#undef	DEBUG_SERIAL_DATA

/*
  Debug output:

  AXX   : ATN 0xXX
  c     : listen_handler cancelled
  C     : CLOSE
  l     : UNLISTEN
  L     : LISTEN
  D     : DATA 0x60
  O     : OPEN 0xfX
  ?XX   : unknown cmd 0xXX
  .     : timeout after ATN

*/

/* -------------------------------------------------------------------------
 *  Error and command channel handling
 */

static void set_ieee_ok(void);

static errormsg_t error = {
	set_ieee_ok
};

static void set_ieee_ok(void) {
	set_error(&error, 0);
};

static void ieee_submit_status_refill(int8_t channelno, packet_t *txbuf, packet_t *rxbuf,
                void (*callback)(int8_t channelno, int8_t errnum)) {

#ifdef DEBUG_SERIAL
	debug_puts("IEEE Status refill"); debug_putcrlf();
#endif

	if (packet_get_type(txbuf) != FS_READ) {
		// should not happen
		debug_printf("SNH: packet type is not FS_READ, but: %d\n", packet_get_type(txbuf));
		callback(channelno, ERROR_NO_CHANNEL);
		return;
	}

	uint8_t *buf = packet_get_buffer(rxbuf);
	uint8_t len = packet_get_capacity(rxbuf);

	strncpy(buf, error.error_buffer, len);
	buf[len-1] = 0;	// avoid buffer overflow

	// overwrite with actual length
	len = strlen(buf);

	// fixup packet	
	packet_set_filled(rxbuf, channelno, FS_EOF, len);

	// reset actual error channel until next read op
	set_error(&error, 0);

	// notify about packet ready
	callback(channelno, 0);
}

static provider_t ieee_status_provider = {
	NULL,
	ieee_submit_status_refill,
	NULL,
	NULL
};


/********************************************************************************/

static uint8_t secaddr_offset_counter;

void bus_init() {
	secaddr_offset_counter = 0;

	set_error(&error, ERROR_DOSVERSION);
}

/* Init IEEE bus */
void bus_init_bus(bus_t *bus) {

	bus->secaddr_offset = secaddr_offset_counter;
	secaddr_offset_counter += 16;

  	/* Read the hardware-set device address */
	//  device_address = device_hw_address();
  	bus->device_address = 8;

  	/* Init vars and flags */
  	bus->command.command_length = 0;

	bus->channel = NULL;
}


/********************************************************************************
 * command buffer and command execution
 */

static volatile bus_t *bus_for_irq;

static void _cmd_callback(int8_t errnum, uint8_t *rxdata) {
    	bus_for_irq->errnum = errnum;
    	if (errnum == 1) {
		if (rxdata != NULL) {
			bus_for_irq->errparam = rxdata[1];
		} else {
			bus_for_irq->errparam = 0;
		}
    	}
    	bus_for_irq->cmd_done = 1;
}

static int16_t cmd_handler (bus_t *bus)
{

	int16_t st = 0;

	bus->cmd_done = 0;
   
	uint8_t secaddr = bus->secondary & 0x0f;

	int8_t rv = 0;

	// prepare for callback from interrupt
	bus_for_irq = bus;
	bus_for_irq->cmd_done = 0;

	if (secaddr == 0x0f) {
      		/* Handle commands */
      		rv = command_execute(bus_secaddr_adjust(bus, secaddr), &(bus->command), &error, 
									_cmd_callback);
    	} else {
      		/* Handle filenames */

#ifdef DEBUG_SERIAL
      		debug_printf("Open file secaddr=%02x, name='%s'\n",
         		secaddr, bus->command.command_buffer);
#endif

      		rv = file_open(bus_secaddr_adjust(bus, secaddr), &(bus->command), &error, 
									_cmd_callback, secaddr == 1);
    	}
      	if (rv < 0) {
		// open ran into an error
		// -- errormsg should be already set, so nothing left to do here
		// TODO
		debug_printf("Received direct error number on open: %d\n", rv);
        	set_error(&error, 74);
		st = 2;
      	}	else {
		// as this code is not (yet?) prepared for async operation, we 
		// need to wait here until the response from the server comes
		// Note: use bus_for_irq here, as it is volatile
		while (bus_for_irq->cmd_done == 0) {
			// TODO this should be reworked more backend (serial) independent
			serial_delay();
		}
		debug_printf("Received callback error number on open: %d\n", bus_for_irq->errnum);
		// result of the open
        	if (bus_for_irq->errnum != 0) {
			if (bus_for_irq->errnum == 1) {
				set_error_ts(&error, bus_for_irq->errnum, bus_for_irq->errparam, 0);
			} else {
        	        	set_error(&error, bus_for_irq->errnum);
			}
        	        channel_close(bus_secaddr_adjust(bus, secaddr));
        	} else {
                	// really only does something on read-only channels
                	channel_preload(bus_secaddr_adjust(bus, secaddr));
		}
      	}

    	bus->command.command_length = 0;

    	return st;
}


/********************************************************************************
 * impedance match
 */

// called during listenloop to send bytes to the server
int16_t bus_sendbyte(bus_t *bus, uint8_t data, uint8_t with_eoi) {

    int16_t st = 0;
#ifdef DEBUG_SERIAL_DATA
    debug_printf("sendbyte: %02x (%c)\n", data, (isprint(data) ? data : '-'));
#endif
//delayus(45);

    if((bus->secondary & 0x0f) == 0x0f || (bus->secondary & 0xf0) == 0xf0) {
      if (bus->command.command_length < CONFIG_COMMAND_BUFFER_SIZE) {
        bus->command.command_buffer[bus->command.command_length++] = data;
      }
    } else {
      bus->channel = channel_put(bus->channel, data, with_eoi);
      if (bus->channel == NULL) {
	st = 0x83;	// TODO correct code
      }
    }
    return st + (bus->device << 8);
}


// called during talkloop to receive bytes from the server
// If the preload parameter is set, the data byte is set,
// but the read pointers are not advanced (used to be named "fake"...)
int16_t bus_receivebyte(bus_t *bus, uint8_t *data, uint8_t preload) {

	int16_t st = 0;

	uint8_t secaddr = bus->secondary & 0x0f;
	channel_t *channel = bus->channel;

	if (channel == NULL) {
		if (secaddr == 15) {
      			channel_open(bus_secaddr_adjust(bus, secaddr), WTYPE_READONLY, &ieee_status_provider, NULL);
			channel = channel_find(bus_secaddr_adjust(bus, secaddr));
			bus->channel = channel;
		}
	}

	if (channel == NULL) {
		// if still NULL, error
		set_error(&error, ERROR_FILE_NOT_OPEN);
		st = 0x83;
	} else {
#ifdef DEBUG_SERIAL
		//debug_printf("rx: chan=%p, channo=%d\n", channel, channel->channel_no);
#endif
		// first fillup the buffers
		channel_preloadp(channel);

		*data = channel_current_byte(channel);
		if (channel_current_is_eof(channel)) {
			debug_printf("EOF!\n");
			st |= 0x40;
		}

		if (!preload) {
			// make sure the next call does have a data byte
			if (!channel_next(channel)) {
				if (channel_has_more(channel)) {
					channel_refill(channel);
				} else {
      					if (secaddr == 15 || secaddr == 0) {
        					// autoclose when load is done, or after reading status channel
						channel_close(bus_secaddr_adjust(bus, secaddr));
						bus->channel = NULL;
					}
				}
			}
		}
	}
	return st + (bus->device << 8);
}

/* These routines work for IEEE488 emulation on both C64 and PET.  */
static int16_t bus_command(bus_t *bus)
{
    uint8_t b;
    int8_t secaddr;
    int st = 0;

#ifdef DEBUG_SERIAL
    debug_printf("***ParallelCommand %02x %02x\n",
         bus->device, bus->secondary);
#endif

    /* which device ? */
    //p = &serialdevices[TrapDevice & 0x0f];
    
    if ((bus->device & 0x0f) != bus->device_address) {
	return 0x80;	// device not present
    }


    secaddr = bus->secondary & 0x0f;

    switch (bus->secondary & 0xf0) {
      case 0x60:
          /* Open Channel */
	  bus->channel = channel_find(bus_secaddr_adjust(bus, secaddr));
	  if (bus->channel == NULL) {
		debug_printf("Did not find channel!\n");
		st |= 0x40;	// TODO correct code?
	  }
          if ((!st) && ((bus->device & 0xf0) == 0x40)) {
	      	// if we should TALK, prepare the first data byte
              	st = bus_receivebyte(bus, &b, 1) & 0xbf;   /* any error, except eof */
          }
          break;
      case 0xE0:
          /* Close File */
          if(secaddr == 15) {
	    // is this correct or only a convenience?
            channel_close_range(bus_secaddr_adjust(bus, 0), bus_secaddr_adjust(bus, 15));
          } else {
            /* Close a single buffer */
            channel_close(bus_secaddr_adjust(bus, secaddr));
          }
          break;

      case 0xF0:
          /* Open File */
	  st = cmd_handler(bus);
          break;

      default:
          debug_printf("Unknown command %02X\n\n", bus->secondary & 0xff);
	  break;
    }
    return (st);
}

int16_t bus_attention(bus_t *bus, uint8_t b) {
    int16_t st = 0;

    // UNLISTEN and it is either open or the command channel
    if (b == 0x3f
        && (((bus->secondary & 0xf0) == 0xf0)
            || ((bus->secondary & 0x0f) == 0x0f))) {
	// then process the command
        st = bus_command(bus);
    } else {

	// not open, not command:
        switch (b & 0xf0) {
          case 0x20:
          case 0x40:
	      // store device number plus LISTEN/TALK info
	      if ((b & 0x0f) == bus->device_address) {
              	bus->device = b;
	      }
              break;

          case 0x60:
          case 0xe0:
	      // secondary address (open DATA channel, or CLOSE)
              bus->secondary = b;
	      // process a command if necessary
              st = bus_command(bus);
              break;

          case 0xf0:            /* Open File needs the filename first */
              bus->secondary = b;
	      // TODO: close previously opened file
              break;
	  default:
	      // falls through e.g. for unlisten/untalk
	      break;
        }
    }

    if ((b == 0x3f) || (b == 0x5f)) {
	// unlisten, untalk
        bus->device = 0;
        bus->secondary = 0;
    } else {
    	if (bus->device_address != (bus->device & 0x0f)) {
		// not this device
        	st |= 0x80;
    	}
    }

#ifdef DEBUG_SERIAL
    debug_printf("ParallelAttention(%02x)->TrapDevice=%02x, st=%04x\n",
               b, bus->device, st + (bus->device << 8));
#endif

    st |= bus->device << 8;

    return st;
}

