
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

#ifdef HAS_IEC

/**
 * Hardware-independent IEC layer 
 */

#include <ctype.h>
#include <delay.h>

#include "iechw.h"
#include "bus.h"
#include "timer.h"

#include "debug.h"
#include "led.h"

#define DEBUG_BUS

// Prototypes

static void talkloop(void);
static void listenloop(void);

#define isListening()   ((ser_status&0xe000)==0x2000)
#define isTalking()     ((ser_status&0xe000)==0x4000)

// bus state
static bus_t bus;

// TODO: make that ... different...
static int16_t ser_status = 0;


/***************************************************************************
 * Listen handling
 */

// check whether we got an ATN; this depends on whether we are 
// under ATN (when receiving commands) or not under ATN (when receiving data)
// returns !=0 when ATN has changed
static uint8_t checkatn(uint8_t underatn) 
{
	if (underatn) {
		return satnishi();
	} else {
		return satnislo();
	}
}

// read a byte from IEEE - E9C9 in VC1541
static int16_t iecin(uint8_t underatn)
{
	uint8_t eoi = 0;
	uint8_t port;
	uint8_t cnt = 8;
	uint8_t data = 0;

	do {
		if (checkatn(underatn)) 
			return -1;
	} while (is_port_clklo(read_debounced()));

	datahi();

	// wait until data is really hi (other devices may delay this)
	do {
		if (checkatn(underatn))
			return -1;
	} while (is_port_datalo(read_debounced()));

	// set timer with 256 us
	timer_set_us(256);

	do {
		if (checkatn(underatn)) {
			return -1;
		}

		if (timer_is_timed_out()) { 
			// handle EOI condition
			datalo();
			// at least 23 cycles + 43+ for bad lines
			delayus(80);

			datahi();

			do {
				if (checkatn(underatn)) {
					return -1;
				}
			} while (is_port_clkhi(read_debounced()));
			
			eoi = 1;

			break;
		}

	} while (is_port_clkhi(read_debounced()));

	// shift in all bits
	do {
		do {
			port = read_debounced();
		} while (is_port_clklo(port));

		data >>= 1;
		if (is_port_datahi(port)) {
			data |= 128;
		}

		do {
			if (checkatn(underatn)) {
				return -1;
			}
		} while (is_port_clkhi(read_debounced()));

		cnt--;

	} while (cnt > 0);

	// this can cause device not present on save
	//delayus(5);

	datalo();
	delayus(256);

	return (0xff & data) | (eoi ? 0x4000 : 0);
}

static void listenloop() {
#ifdef DEBUG_BUS
	debug_putc('L');
#endif

	int16_t c;

	do {
		// disable interrupts
		cli();
		// read byte from IEC
		c = iecin(0);
		// enable ints
		sei();
		if (c < 0) {
//debug_printf("c<0: %d\n", c);
			break;
		}
//debug_printf("sending byte: %02x\n", c&0xff);
            	ser_status = bus_sendbyte(&bus, c, BUS_SYNC | ((c & 0x4000) ? BUS_FLUSH : 0));
        } while (1);

	return;
}

/***************************************************************************/

// more info see here: https://groups.google.com/forum/?hl=de&fromgroups=#!msg/comp.sys.cbm/e4qxrtt5RP0/0q1EVUkV8moJ

static int16_t iecout(uint8_t data, uint8_t witheoi) {
	
	uint8_t port;
	uint8_t cnt = 8;

	if (checkatn(0)) {
		return -1;
	}

	// just in case, release the data line
	datahi();

	// make sure data is actually lo
	do {
		if (checkatn(0)) {
			return -1;
		}
//led_toggle();
	} while (is_port_datahi(read_debounced()));

	
	// e91f
	clkhi();

	// wait for the listener to release data, signalling ready for data
	do {
		if (checkatn(0)) {
			return -1;
		}
	} while (is_port_datalo(read_debounced()));

	if (witheoi) {
		// signal the EOI
		// wait for data low as acknowledge from the listener
		do {
			if (checkatn(0)) {
				return -1;
			}
		} while (is_port_datahi(read_debounced()));

		// and wait for DATA to go back up
		do {
			if (checkatn(0)) {
				return -1;
			}
		} while (is_port_datalo(read_debounced()));

		// done signalling EOI
	}

	// e94b
	clklo();
	
	// e958
	do {
		// e95c
		if (is_port_datalo(read_debounced())) {
			return -1;
		}

		if (data & 1) {
			datahi();
		} else {
			datalo();
		}
		data >>= 1;

		delayus(80);

		clkhi();

		// C64: min about 15 cycles to detect it, plus 43+ bad line cycles
		// fef3
		delayus(70);

		// fefb
		clklo();
		datahi();

		cnt--;
	} while (cnt > 0);


	// wait for the host to set data lo
	do {
		if (checkatn(0)) {
			return -1;
		}
	} while (is_port_datahi(read_debounced()));	

	return 0;
}

static void talkloop()
{
        int16_t er /*,sec*/;
        uint8_t c;

	do {
            	ser_status = bus_receivebyte(&bus, &c, 1);
//debug_printf("reading byte from bus: %02x, ser_status=%04x\n", c,ser_status);debug_flush();

		// disable ints
		cli();
		// send byte to IEC
		er = iecout(c, ser_status & 0x40);
		// enable ints
		sei();

//		delayus(4000);

//debug_printf("next, er=%d, ser_status=%04x\n", er, ser_status);debug_flush();

		if (er >= 0) {
            		ser_status = bus_receivebyte(&bus, &c, 0);
		}
//debug_printf("commited, er=%d, ser_status=%04x\n", er, ser_status);debug_flush();

	} while (er >= 0);

debug_printf("bailing out with er=%d\n", er);
}

/***************************************************************************
 * main IEEE "loop"
 * The actual loop is outside this method to handle other functions outside
 * the IEEE stuff. The outside loop calls this one on each iteration.
 */

void iec_mainloop_iteration(void)
{
        int16_t cmd = 0;

        ser_status=0;

	// only do something on ATN low
	if (satnishi()) {
		return;
	}

	clkhi();

	datalo();

	// acknowledge ATN
	satnalo();

	// on the C64, CLK is set high directly after setting ATN
	// but here we are possibly faster than that, so do a delay
	delayus(20);

        // Loop to get commands during ATN lo ----------------------------

	do {
		// disable ints
		cli();
		// get byte (under ATN) - call to E9C9
		cmd = iecin(1);
		// enable ints again
		sei();

		if (cmd < 0) {
			break;
		}
		ser_status = bus_attention(&bus, 0xff & cmd);

	} while (satnislo());
	
        // ---------------------------------------------------------------
	// ATN is high now
	// parallelattention has set status what to do
	// now transfer the data
cmd:

#ifdef DEBUG_BUS
	debug_printf("stat=%04x", ser_status); debug_putcrlf();
#endif
	
	// E8D7
	satnahi();

	if(isListening())
        {
		listenloop();
        } else
        {
		if (isTalking()) {
			datahi();
			clklo();
			talkloop();
		}
        }

	iechw_setup();
#ifdef DEBUG_BUS
	debug_putc('X'); debug_flush();
#endif	
        return;
}

/***************************************************************************
 * Init code
 */
void iec_init(uint8_t deviceno) {

        iechw_setup();

	// register bus instance
	bus_init_bus(&bus);
}

#endif // HAS_IEC
