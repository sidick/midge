#ifndef MIDGE_TRANSPORT_AMISSL_H
#define MIDGE_TRANSPORT_AMISSL_H

#include <stdint.h>

#include <exec/types.h> /* struct Library, ULONG */

#include <openssl/ssl.h>

#include "mqtt_transport.h"

typedef struct {
    int fd;
    int ctrl_c; /* set by send()/recv() when a wait was broken by
                   SIGBREAKF_CTRL_C - see transport_bsdsocket.h's ctrl_c */

    /* Per-connection library bases - never file-scope statics/globals (see
       transport_bsdsocket.h's socket_base comment; the same CLAUDE.md
       static/extern trap applies here). AmiSSL's own Autodoc confirms this
       is the intended usage: "each opener gets their own unique baserel
       based AmiSSLBase" (amissl.library/--background--), and the NDK's
       inline/amissl.h macros expand against whatever C identifier named
       AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase is lexically in scope at
       the call site (AMISSL_BASE_NAME defaults to the bare identifier),
       exactly like proto/bsdsocket.h's SocketBase - so every function in
       transport_amissl.c that calls a bsdsocket or AmiSSL/OpenSSL function
       declares its own local shadow initialised from these fields. */
    struct Library *socket_base;
    struct Library *amissl_master_base;
    struct Library *amissl_base;
    struct Library *amissl_ext_base;

    SSL_CTX *ssl_ctx;
    SSL *ssl;

    ULONG break_sigmask; /* extra WaitSelect() wakeup signal(s) - see
                   transport_bsdsocket.h. Zeroed by transport_amissl_connect;
                   a caller that wants an extra wakeup source sets this
                   field afterwards. */
} amissl_ctx;

/* Opens bsdsocket.library and amisslmaster.library for this task, connects
 * a non-blocking TCP socket to host:port, and performs an AmiSSL/OpenSSL
 * TLS handshake over it, gated by WaitSelect() (SIGBREAKF_CTRL_C in the
 * mask, same discipline as transport_bsdsocket_connect). Certificate and
 * hostname verification are on by default; pass insecure_skip_verify
 * nonzero to disable both (SSL_VERIFY_NONE) - for testing against
 * self-signed brokers only, never for production use. `ca_file`, if
 * non-NULL, additionally loads a PEM file as an extra trust anchor
 * (SSL_CTX_load_verify_locations()) - for a private CA not in AmiSSL's
 * bundled trust store (issue #13); ignored when insecure_skip_verify is
 * set (nothing to verify against then).
 *
 * Requires an AmiSSL: assign (see amissl.library's install docs - the
 * amissl package assigns it to SYS:Devs/AmiSSL) for InitAmiSSL()'s cert
 * store/config, even with SSL_VERIFY_NONE.
 *
 * Wires `out` up via `ctx_storage` (caller-owned, no allocation) and
 * zeroes ctx_storage->break_sigmask. Returns 0 on success, -1 on failure
 * (no TCP/IP stack, no AmiSSL, resolve failure, connect failure, or
 * handshake failure) - no resources leaked on any error path. */
int transport_amissl_connect(mqtt_transport *out, amissl_ctx *ctx_storage,
                              const char *host, uint16_t port,
                              int insecure_skip_verify,
                              const char *ca_file);

#endif
