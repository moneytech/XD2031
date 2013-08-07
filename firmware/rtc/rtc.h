#ifndef RTC_H
#define RTC_H

#include <inttypes.h>

#ifdef PCTEST
    typedef char* errormsg_t;
    enum { CBM_ERROR_OK, CBM_ERROR_READ, CBM_ERROR_DRIVE_NOT_READY,
           CBM_ERROR_SYNTAX_UNKNOWN, CBM_ERROR_WRITE_ERROR };
#define set_status(dummy, msg) puts(msg)
#else
#   include "errormsg.h"
#endif

typedef struct {
    uint16_t   year;   /* 2000..2099 */
    uint8_t    month;  /* 1..12 */
    uint8_t    mday;   /* 1.. 31 */
    uint8_t    wday;   /* 1..7, 1=Monday, 2=Tuesday... */
    uint8_t    hour;   /* 0..23 */
    uint8_t    min;    /* 0..59 */
    uint8_t    sec;    /* 0..59 */
} RTC_t;


#ifdef HAS_RTC
int8_t rtc_init (void);                    // Initialize RTC
int8_t rtc_gettime (RTC_t* datim);         // Get time
int8_t rtc_settime (const RTC_t* datim);   // Set time

// parser for time commands
int8_t rtc_time(char *p, errormsg_t* errormsg);

// printf a time
void rtc_timestamp(const RTC_t* datim);
#else

static inline int8_t rtc_init(void) {
    return 0;
}

static inline int8_t rtc_gettime (RTC_t* x) {
    *x = (RTC_t) { 2012, 4, 8, 7, 6, 50, 20 };
    return 1;
}

static inline int8_t rtc_settime(const RTC_t* x) {
    return -1;
}

#endif

#endif