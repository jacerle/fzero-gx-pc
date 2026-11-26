/* Native OS library: console, panic, time, MEM1 arena and interrupt masking. */

#include "sdk/sdk_internal.h"

#include "fzgx_port/platform_api.h"
#include "fzgx_port/ppc_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FZGX_OS_ARENA_ALIGN 32u

static u8 *s_arena_alloc; /* malloc'd block backing the arena */
static void *s_arena_lo;
static void *s_arena_hi;

void OSInit(void) {
    u8 *base;
    if (s_arena_alloc == NULL) {
        s_arena_alloc = (u8 *)malloc(FZGX_OS_ARENA_SIZE + FZGX_OS_ARENA_ALIGN);
        if (s_arena_alloc == NULL) {
            OSPanic(__FILE__, __LINE__, "OSInit: cannot allocate %u byte arena\n", (unsigned)FZGX_OS_ARENA_SIZE);
        }
    }
    /* Every boot starts with the whole arena, as after a retail reset. Bounds are 32-byte
     * aligned; game allocators round within them. Retail __OSPSInit also clears the GQRs. */
    memset(fzgx_gqr, 0, sizeof(fzgx_gqr));
    base = s_arena_alloc + ((FZGX_OS_ARENA_ALIGN - ((uintptr_t)s_arena_alloc % FZGX_OS_ARENA_ALIGN)) %
                            FZGX_OS_ARENA_ALIGN);
    s_arena_lo = base;
    s_arena_hi = base + FZGX_OS_ARENA_SIZE;
}

void OSReport(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    vfprintf(stdout, msg, args);
    va_end(args);
    fflush(stdout);
}

void OSPanic(const char *file, int line, const char *msg, ...) {
    va_list args;
    fflush(stdout);
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fprintf(stderr, " in \"%s\" on line %d.\n", file, line);
    fflush(stderr);
    abort();
}

void *OSGetArenaLo(void) {
    return s_arena_lo;
}

void *OSGetArenaHi(void) {
    return s_arena_hi;
}

void OSSetArenaLo(void *newLo) {
    s_arena_lo = newLo;
}

void OSSetArenaHi(void *newHi) {
    s_arena_hi = newHi;
}

BOOL OSDisableInterrupts(void) {
    return fzgx_sdk_set_interrupts_enabled(FALSE);
}

BOOL OSEnableInterrupts(void) {
    return fzgx_sdk_set_interrupts_enabled(TRUE);
}

BOOL OSRestoreInterrupts(BOOL level) {
    return fzgx_sdk_set_interrupts_enabled(level);
}

OSTime OSGetTime(void) {
    fzgx_sdk_pump(); /* time-polling loops are where retail code observes completions */
    return (OSTime)fzgx_os_time_ticks();
}

OSTick OSGetTick(void) {
    fzgx_sdk_pump();
    return (OSTick)fzgx_os_time_ticks();
}

/* Days since 1970-01-01 to a proleptic Gregorian date (H. Hinnant's civil_from_days). */
static void civil_from_days(s64 days, s32 *year, s32 *month, s32 *mday) {
    const s64 z = days + 719468;
    const s64 era = (z >= 0 ? z : z - 146096) / 146097;
    const s64 doe = z - era * 146097;
    const s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const s64 mp = (5 * doy + 2) / 153;
    const s64 m = mp < 10 ? mp + 3 : mp - 9;
    *year = (s32)(yoe + era * 400 + (m <= 2));
    *month = (s32)m; /* 1..12 */
    *mday = (s32)(doy - (153 * mp + 2) / 5 + 1);
}

static BOOL is_leap_year(s32 year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime *td) {
    static const s32 kDaysBeforeMonth[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const s64 ticks_per_sec = OS_TIMER_CLOCK;
    s64 secs = ticks / ticks_per_sec;
    s64 sub = ticks % ticks_per_sec;
    s64 days;
    s64 sec_of_day;
    s32 year;
    s32 month;
    s32 mday;

    if (sub < 0) {
        sub += ticks_per_sec;
        secs -= 1;
    }
    /* sub ticks -> us: 40.5 ticks per us = 81 per 2 us */
    td->usec = (int)((sub * 2 / 81) % 1000);
    td->msec = (int)((sub * 2 / 81) / 1000);

    days = secs / 86400;
    sec_of_day = secs % 86400;
    if (sec_of_day < 0) {
        sec_of_day += 86400;
        days -= 1;
    }
    td->hour = (int)(sec_of_day / 3600);
    td->min = (int)(sec_of_day / 60 % 60);
    td->sec = (int)(sec_of_day % 60);

    /* OSTime epoch is 2000-01-01, 10957 days after 1970-01-01 (a Thursday, wday 4). */
    days += 10957;
    td->wday = (int)(((days + 4) % 7 + 7) % 7);
    civil_from_days(days, &year, &month, &mday);
    td->year = year;
    td->mon = month - 1;
    td->mday = mday;
    td->yday = kDaysBeforeMonth[month - 1] + mday - 1 + ((month > 2 && is_leap_year(year)) ? 1 : 0);
}
