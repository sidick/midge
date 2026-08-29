#ifndef MIDGE_MQTT_TRANSPORT_H
#define MIDGE_MQTT_TRANSPORT_H

/* The one seam between src/core (portable) and everything platform-specific
 * (src/host, src/amiga - and later the AmiSSL/OpenSSL TLS transports in
 * Phase 3). mqtt_client only ever talks to a connection through this vtable;
 * it never touches a socket, a library base, or a clock. */

#include <stddef.h>
#include <stdint.h>

typedef struct mqtt_transport {
    void *ctx;

    /* Sends up to `len` bytes from `buf`. Returns the number of bytes
     * actually sent (may be less than `len` on a partial write), 0 if
     * nothing could be sent right now (would-block, not an error), or a
     * negative value on a fatal transport error. */
    int (*send)(void *ctx, const uint8_t *buf, size_t len);

    /* Reads up to `cap` bytes into `buf`. Returns the number of bytes
     * actually read, 0 if none are available right now (would-block, not
     * an error - includes a client-side connect() that has not resolved
     * yet), or a negative value if the connection has failed or closed. */
    int (*recv)(void *ctx, uint8_t *buf, size_t cap);

    /* Releases the transport's resources (closes the socket). Idempotent
     * is not required - called at most once, when the client is done. */
    void (*close)(void *ctx);
} mqtt_transport;

#endif
