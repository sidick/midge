#ifndef MIDGE_TRANSPORT_BSD_H
#define MIDGE_TRANSPORT_BSD_H

#include <stdint.h>

#include "mqtt_transport.h"

typedef struct {
    int fd;
} bsd_ctx;

/* Resolves host:port, connects a blocking TCP socket with a short receive
 * timeout (so a caller pumping mqtt_client_process() gets periodic wakeups
 * to check keepalive scheduling without a real non-blocking event loop),
 * and wires `out` up to it via `ctx_storage` (caller-owned, no allocation).
 * Returns 0 on success, -1 on failure. */
int transport_bsd_connect(mqtt_transport *out, bsd_ctx *ctx_storage,
                           const char *host, uint16_t port);

#endif
