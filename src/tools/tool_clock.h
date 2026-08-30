#ifndef MIDGE_TOOL_CLOCK_H
#define MIDGE_TOOL_CLOCK_H

#include <stdint.h>

/* A millisecond counter, used as the `now_ms` the CLI tools pump
 * mqtt_client with (see mqtt_client.h) for keepalive/timeout scheduling.
 * One implementation per platform:
 *
 *   src/host/clock.c   clock_gettime(CLOCK_MONOTONIC, ...) - immune to
 *                       wall-clock adjustments (NTP, DST, a manual clock
 *                       set), which matters here: this value only ever
 *                       feeds elapsed-time subtraction, never anything
 *                       date-like.
 *   src/amiga/clock.c  dos.library DateStamp() - NOT time(): under some
 *                       AROS/Copperline HostSocket configurations, libnix's
 *                       time() was found to hang indefinitely before ever
 *                       returning (see docs/ARCHITECTURE.md's testing
 *                       notes), apparently entangled with its ANSI-locale
 *                       auto-load at first call. DateStamp() is dos.library's
 *                       own native clock, always available, no such
 *                       dependency - BUT, unlike the host side, it is
 *                       wall-clock, not monotonic (classic AmigaOS has no
 *                       cheap equivalent of CLOCK_MONOTONIC - see issue
 *                       #8). A clock change mid-session (SetClock, an
 *                       NTP-driven utility, a timezone/DST correction)
 *                       skews every keepalive/PUBACK/SUBACK/backoff
 *                       deadline computed from it: stepping the clock
 *                       either direction makes the next elapsed-time
 *                       subtraction wrap to a huge unsigned value,
 *                       triggering an immediate spurious timeout - there
 *                       is no way to distinguish that from 49 days of
 *                       genuine `uint32_t` wraparound from the delta
 *                       alone. A real fix needs a genuinely monotonic
 *                       source (timer.device's TR_GETSYSTIME is the
 *                       candidate), which needs a per-task IORequest -
 *                       i.e. state threaded through every caller of this
 *                       otherwise-stateless function, not a drop-in
 *                       replacement. Deliberately deferred (issue #8):
 *                       accepted as a documented caveat rather than an
 *                       invasive API change, since a clock actively
 *                       changing mid-session is rare in practice.
 *
 * Absolute value and epoch don't matter - callers only ever compare two
 * readings' difference. */
uint32_t tool_now_ms(void);

#endif
