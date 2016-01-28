/**************************************************************************

    device.h -- device dependant definitions

    This file is part of XD-2031 -- Serial line filesystem server for CBMs

    Copyright (C) 2012 Andre Fachat <afachat@gmx.de>
    Copyrifht (C) 2012 Nils Eilers  <nils.eilers@gmx.de>

    XD-2031 is free software: you can redistribute it and/or
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

**************************************************************************/

#ifndef DEVICE_H
#define DEVICE_H

#include "hwdefines.h"

// init device
void device_init(void);

#ifndef __AVR__
void device_setup(int argc, const char* argv[]);
#endif

// one polling loop iteration for the device
void device_loop(void);

// lock device, so no commands come in from the IEC bus
void device_lock(void);
// unlock device, so commands can come in from the IEC bus
void device_unlock(void);


#endif