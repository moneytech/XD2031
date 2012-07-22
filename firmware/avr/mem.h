
#include <avr/pgmspace.h>

#define	IN_ROM		PROGMEM

#define	IN_ROM_STR(s)	PSTR(s)

#define	rom_strlen(s)		strlen_P(s)
#define	rom_memcpy(t, s, l)	memcpy_P(t, s, l)
#define	rom_vsprintf(s, f, ...)	vsprintf_P(s, f, __VA_ARGS__)
