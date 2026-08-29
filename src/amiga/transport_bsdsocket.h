#ifndef MIDGE_TRANSPORT_BSDSOCKET_H
#define MIDGE_TRANSPORT_BSDSOCKET_H

#include <stdint.h>

#include <exec/types.h> /* struct Library, ULONG */

#include "mqtt_transport.h"

typedef struct {
    int fd;
    int ctrl_c; /* set by recv() when a wait was broken by SIGBREAKF_CTRL_C
                   (see transport_bsdsocket.c) - the Amiga main checks this
                   after a transport error to print "stopped" rather than
                   a generic connection-lost message */
    struct Library *socket_base; /* this connection's bsdsocket.library base
                   (per docs/ARCHITECTURE.md - handles are per-task, never
                   shared across processes). Opened by
                   transport_bsdsocket_connect(), closed by bsdsocket_close().
                   Lives in the ctx rather than as a file-scope global so
                   mqtt.library (one shared data segment, libnix libinit.o -
                   see CLAUDE.md's static/extern trap) can host several
                   simultaneous connections without them trampling each
                   other's library base. */
    ULONG break_sigmask; /* extra signal(s), beyond SIGBREAKF_CTRL_C, that
                   should wake recv()'s WaitSelect() poll early - e.g. a
                   library connection subprocess's command MsgPort signal.
                   Zeroed by transport_bsdsocket_connect(); a caller that
                   wants an extra wakeup source sets this field afterwards,
                   before driving the connection. Plain CLI callers never
                   touch it and get today's SIGBREAKF_CTRL_C-only wait. */
} bsdsocket_ctx;

/* Opens bsdsocket.library for this task (per docs/ARCHITECTURE.md - handles
 * are per-task, never shared across processes), resolves host:port, and
 * connects a TCP socket. Wires `out` up via `ctx_storage` (caller-owned, no
 * allocation) and zeroes ctx_storage->break_sigmask - set that field
 * afterwards if recv() should also wake on a signal beyond
 * SIGBREAKF_CTRL_C. Returns 0 on success, -1 on failure (no TCP/IP stack,
 * resolve failure, connect failure). */
int transport_bsdsocket_connect(mqtt_transport *out, bsdsocket_ctx *ctx_storage,
                                 const char *host, uint16_t port);

#endif
