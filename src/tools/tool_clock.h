#ifndef MIDGE_TOOL_CLOCK_H
#define MIDGE_TOOL_CLOCK_H

#include <stdint.h>

/* A monotonic-ish millisecond counter, used as the `now_ms` the CLI tools
 * pump mqtt_client with (see mqtt_client.h) for keepalive/timeout
 * scheduling. One implementation per platform:
 *
 *   src/host/clock.c   time() - fine on every host libc.
 *   src/amiga/clock.c  dos.library DateStamp() - NOT time(): under some
 *                       AROS/Copperline HostSocket configurations, libnix's
 *                       time() was found to hang indefinitely before ever
 *                       returning (see docs/ARCHITECTURE.md's testing
 *                       notes), apparently entangled with its ANSI-locale
 *                       auto-load at first call. DateStamp() is dos.library's
 *                       own native clock, always available, no such
 *                       dependency.
 *
 * Absolute value and epoch don't matter - callers only ever compare two
 * readings' difference. */
uint32_t tool_now_ms(void);

#endif
