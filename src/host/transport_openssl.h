#ifndef MIDGE_TRANSPORT_OPENSSL_H
#define MIDGE_TRANSPORT_OPENSSL_H

#include <stdint.h>

#include <openssl/ssl.h>

#include "mqtt_transport.h"

typedef struct {
    int fd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
} openssl_ctx;

/* Resolves host:port, connects a blocking TCP socket with a short receive
 * timeout (same rationale as transport_bsd_connect - periodic wakeups for
 * mqtt_client_process()), then performs a TLS handshake over it. Certificate
 * and hostname verification are on by default (system trust store via
 * SSL_CTX_set_default_verify_paths()); pass insecure_skip_verify nonzero to
 * disable both (SSL_VERIFY_NONE) - for testing against self-signed brokers
 * only, never for production use. Wires `out` up via `ctx` (caller-owned, no
 * allocation). Returns 0 on success, -1 on failure (no resources leaked on
 * any error path). */
int transport_openssl_connect(mqtt_transport *out, openssl_ctx *ctx,
                               const char *host, uint16_t port,
                               int insecure_skip_verify);

#endif
