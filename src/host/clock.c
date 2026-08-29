/* glibc hides clock_gettime()/CLOCK_MONOTONIC (POSIX.1-2001) under a strict
 * -std=c99 build unless a feature-test macro says otherwise; must be
 * defined before the first system header - see transport_bsd.c's longer
 * note (same root cause, same CI-only failure on Linux, not local macOS
 * development, where libc exposes these unconditionally). */
#define _POSIX_C_SOURCE 200112L

#include "tool_clock.h"

#include <time.h>

uint32_t tool_now_ms(void)
{
    struct timespec ts;

    /* CLOCK_MONOTONIC, not time(NULL)*1000: a wall-clock adjustment (NTP
     * step, DST, manual clock set) must never make elapsed-time math (the
     * CONNACK/keepalive deadlines this feeds) run backwards or jump. Modern
     * glibc (>=2.17) and macOS's libc both provide clock_gettime() without
     * linking -lrt. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                       (uint64_t)ts.tv_nsec / 1000000u);
}
