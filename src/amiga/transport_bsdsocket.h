#ifndef MIDGE_TRANSPORT_BSDSOCKET_H
#define MIDGE_TRANSPORT_BSDSOCKET_H

#include <stdint.h>

#include "mqtt_transport.h"

typedef struct {
    int fd;
    int ctrl_c; /* set by recv() when a wait was broken by SIGBREAKF_CTRL_C
                   (see transport_bsdsocket.c) - the Amiga main checks this
                   after a transport error to print "stopped" rather than
                   a generic connection-lost message */
} bsdsocket_ctx;

/* Opens bsdsocket.library for this task (per docs/ARCHITECTURE.md - handles
 * are per-task, never shared across processes), resolves host:port, and
 * connects a TCP socket. Wires `out` up via `ctx_storage` (caller-owned, no
 * allocation). Returns 0 on success, -1 on failure (no TCP/IP stack,
 * resolve failure, connect failure). */
int transport_bsdsocket_connect(mqtt_transport *out, bsdsocket_ctx *ctx_storage,
                                 const char *host, uint16_t port);

#endif
