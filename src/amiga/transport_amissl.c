/* transport_amissl.c — AmigaOS mqtt_transport over AmiSSL/OpenSSL. Structural
 * sibling of transport_bsdsocket.c (TCP connect loop, per-connection library
 * bases, WaitSelect with SIGBREAKF_CTRL_C) and src/host/transport_openssl.c
 * (TLS setup/handshake shape) - see both for the patterns this follows.
 *
 * Derived from tests/copperline/amissl-spike/amisslspike.c (issue #3
 * Milestone 2), which validated that this SSL_set_fd() + WaitSelect()-gated
 * approach works on m68k/OS3 and round-trips a real MQTT CONNECT/CONNACK.
 * Two differences from the spike, both required for production use:
 *
 *   1. No AmiSSLBase/AmiSSLExtBase/AmiSSLMasterBase file-scope globals -
 *      see transport_amissl.h's comment. The spike used real globals as an
 *      acceptable single-connection shortcut; mqtt.library runs several
 *      connection subprocesses against one shared code+data segment, so a
 *      global here would let two simultaneous TLS connections trample each
 *      other's library base the same way a global SocketBase would
 *      (CLAUDE.md). AmiSSL's own Autodoc confirms per-opener bases are the
 *      supported/intended usage, not a hack.
 *
 *   2. send()/recv() gate internally on WANT_READ/WANT_WRITE (one bounded
 *      WaitSelect() wait, then return progress-so-far) rather than
 *      returning 0 immediately like transport_openssl.c's host transport
 *      does. This is NOT optional here: src/core/mqtt_client.c's send_all()
 *      busy-spins on a 0 return with zero backoff (its
 *      MQTT_SEND_MAX_SPINS comment explicitly assumes "blocking-mode
 *      sockets"). transport_openssl.c gets away with immediate-0-return
 *      because its socket is blocking, so SSL_write()/SSL_read() there
 *      essentially never surface WANT_READ/WANT_WRITE in practice. This
 *      transport's socket is non-blocking (needed so recv() stays
 *      SIGBREAKF_CTRL_C-abortable per CLAUDE.md, matching
 *      transport_bsdsocket_recv), so AmiSSL genuinely does return
 *      WANT_READ/WANT_WRITE - an immediate 0 here would make send_all()
 *      hammer SSL_write() up to 1000 times with no real I/O in between,
 *      burning the whole spin budget in microseconds without ever letting
 *      the fd become ready. */

/* NOTE (issue #3): a genuinely stock, unaccelerated 68020 (~14MHz) has been
 * found to intermittently fail on the write immediately following a
 * successful handshake (SSL_ERROR_SYSCALL), while >=16MHz is reliable in
 * every de-risking run tried - matching AmiSSL upstream's own diagnosis of
 * an equivalent symptom (github.com/jens-maus/amissl/issues/111) as a
 * CPU-speed limit, not a fixable library bug. This transport does not paper
 * over that: a persistent SSL_ERROR_SYSCALL is reported as a fatal
 * transport error (-1) like any other, so the client's existing
 * reconnect-with-backoff logic (see mqtt_client.h) is what recovers from
 * it, rather than this file silently retrying past a real failure. See
 * userdocs/CLI-Reference.md's "A note on TLS and CPU speed". */

#include "transport_amissl.h"

#include <exec/types.h>
#include <dos/dos.h> /* SIGBREAKF_CTRL_C */
#include <libraries/amisslmaster.h> /* AMISSL_CURRENT_VERSION */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <proto/amisslmaster.h>
#include <amissl/tags.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h> /* FIONBIO */

#include <string.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>

/* WaitSelect() poll interval per gated retry - matches
 * transport_bsdsocket.c's MQTT_BSDSOCKET_POLL_SECS and
 * tests/copperline/amissl-spike/amisslspike.c's SPIKE_POLL_SECS, so all
 * three Amiga-side network waits share one wakeup cadence. */
#define MQTT_AMISSL_POLL_SECS 1

/* Single bounded wait for readability/writability, mirroring
 * amisslspike.c's wait_gate() exactly (same fd_set/timeval/sigmask shape as
 * transport_bsdsocket_recv). Returns 0 if the fd may now be ready (timeout
 * or genuinely ready - either way the caller retries the SSL call), -1 to
 * abort (ctrl-C, the caller's break_sigmask, or a genuine WaitSelect()
 * failure that must not be treated as "no data" or the caller would
 * busy-loop at 100% CPU). */
static int wait_gate(amissl_ctx *c, int want_write)
{
    struct Library *SocketBase = c->socket_base;
    struct timeval tv;
    fd_set fds;
    ULONG sigmask = SIGBREAKF_CTRL_C | c->break_sigmask;
    long n;

    FD_ZERO(&fds);
    FD_SET(c->fd, &fds);
    tv.tv_sec = MQTT_AMISSL_POLL_SECS;
    tv.tv_usec = 0;

    if (want_write)
        n = WaitSelect(c->fd + 1, NULL, &fds, NULL, &tv, &sigmask);
    else
        n = WaitSelect(c->fd + 1, &fds, NULL, NULL, &tv, &sigmask);

    if (n < 0) {
        if (sigmask & SIGBREAKF_CTRL_C) {
            c->ctrl_c = 1;
            return -1;
        }
        if (sigmask & c->break_sigmask)
            return 0; /* caller's own wakeup source - treat like a timeout */
        return -1; /* genuine WaitSelect failure */
    }
    return 0;
}

static int amissl_send(void *vctx, const uint8_t *buf, size_t len)
{
    amissl_ctx *c = (amissl_ctx *)vctx;
    /* AmiSSLExtBase (the extended v5 API) isn't needed by SSL_write/
       SSL_read's macro expansion - only AmiSSLBase is - so it's not
       shadowed here; see the ctx field comment in transport_amissl.h for
       why it's still retrieved and stored per-connection regardless. */
    struct Library *AmiSSLBase = c->amissl_base;
    int n;

    ERR_clear_error();
    n = SSL_write(c->ssl, buf, (int)len);
    if (n > 0)
        return n;

    {
        int err = SSL_get_error(c->ssl, n);
        if (err == SSL_ERROR_WANT_READ)
            return wait_gate(c, 0) < 0 ? -1 : 0;
        if (err == SSL_ERROR_WANT_WRITE)
            return wait_gate(c, 1) < 0 ? -1 : 0;
        return -1; /* SSL_ERROR_SYSCALL/SSL_ERROR_SSL/ZERO_RETURN - fatal,
                      see this file's top-of-file NOTE (issue #3) */
    }
}

static int amissl_recv(void *vctx, uint8_t *buf, size_t cap)
{
    amissl_ctx *c = (amissl_ctx *)vctx;
    /* AmiSSLExtBase (the extended v5 API) isn't needed by SSL_write/
       SSL_read's macro expansion - only AmiSSLBase is - so it's not
       shadowed here; see the ctx field comment in transport_amissl.h for
       why it's still retrieved and stored per-connection regardless. */
    struct Library *AmiSSLBase = c->amissl_base;
    int n;

    ERR_clear_error();
    n = SSL_read(c->ssl, buf, (int)cap);
    if (n > 0)
        return n;

    {
        int err = SSL_get_error(c->ssl, n);
        if (err == SSL_ERROR_WANT_READ)
            return wait_gate(c, 0) < 0 ? -1 : 0;
        if (err == SSL_ERROR_WANT_WRITE)
            return wait_gate(c, 1) < 0 ? -1 : 0;
        return -1; /* SSL_ERROR_SYSCALL/SSL_ERROR_SSL - fatal; ZERO_RETURN
                      (clean TLS shutdown) also fatal - the peer is gone */
    }
}

static void amissl_close(void *vctx)
{
    amissl_ctx *c = (amissl_ctx *)vctx;
    /* AmiSSLExtBase not shadowed - CloseAmiSSL() only expands against
       AmiSSLMasterBase, see the comment in amissl_send() above. */
    struct Library *AmiSSLBase = c->amissl_base;
    struct Library *AmiSSLMasterBase = c->amissl_master_base;
    struct Library *SocketBase = c->socket_base;

    if (c->ssl != NULL) {
        SSL_shutdown(c->ssl); /* one call, best-effort, same as transport_openssl.c */
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ssl_ctx != NULL) {
        SSL_CTX_free(c->ssl_ctx);
        c->ssl_ctx = NULL;
    }
    if (c->amissl_base != NULL) {
        CloseAmiSSL();
        c->amissl_base = NULL;
        c->amissl_ext_base = NULL;
    }
    if (c->amissl_master_base != NULL) {
        CloseLibrary(AmiSSLMasterBase);
        c->amissl_master_base = NULL;
    }
    if (c->fd >= 0) {
        CloseSocket(c->fd);
        c->fd = -1;
    }
    if (SocketBase != NULL) {
        CloseLibrary(SocketBase);
        c->socket_base = NULL;
    }
}

int transport_amissl_connect(mqtt_transport *out, amissl_ctx *ctx,
                              const char *host, uint16_t port,
                              int insecure_skip_verify,
                              const char *ca_file)
{
    struct Library *SocketBase;
    struct Library *AmiSSLMasterBase;
    /* AmiSSLExtBase not shadowed here either - none of SSL_CTX_new/SSL_new/
       SSL_set_fd/SSL_do_handshake's macro expansions need it - but it is
       still retrieved into ctx->amissl_ext_base below so a caller in a
       later phase (e.g. an extended-API call added to amissl_send/recv)
       can shadow it from there without re-plumbing OpenAmiSSLTags(). */
    struct Library *AmiSSLBase = NULL;
    struct sockaddr_in addr;
    unsigned long ip;
    int fd = -1;
    long one = 1;

    ctx->fd = -1;
    ctx->ctrl_c = 0;
    ctx->socket_base = NULL;
    ctx->amissl_master_base = NULL;
    ctx->amissl_base = NULL;
    ctx->amissl_ext_base = NULL;
    ctx->ssl_ctx = NULL;
    ctx->ssl = NULL;
    ctx->break_sigmask = 0;

    /* --- 1. bsdsocket.library, resolve, connect, non-blocking ----------- */
    SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 4);
    if (!SocketBase)
        return -1; /* no TCP/IP stack */
    ctx->socket_base = SocketBase;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    ip = inet_addr((STRPTR)host);
    if (ip != (unsigned long)-1) {
        addr.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he || !he->h_addr_list[0])
            goto fail_socket;
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        goto fail_socket;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto fail_fd;
    if (IoctlSocket(fd, FIONBIO, (char *)&one) < 0)
        goto fail_fd;
    ctx->fd = fd;

    /* --- 2. amisslmaster.library + OpenAmiSSLTags -----------------------
     * AmiSSL_GetAmiSSLBase/GetAmiSSLExtBase point straight at this
     * connection's own ctx fields - see transport_amissl.h. AmiSSL_SocketBase
     * shares this connection's bsdsocket base with AmiSSL's own internal
     * socket calls, per amissl.library/InitAmiSSLA's AmiSSL_SocketBase tag. */
    AmiSSLMasterBase = OpenLibrary((STRPTR) "amisslmaster.library", 5);
    if (!AmiSSLMasterBase)
        goto fail_fd;
    ctx->amissl_master_base = AmiSSLMasterBase;

    {
        LONG rc = OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
            AmiSSL_UsesOpenSSLStructs, FALSE,
            AmiSSL_GetAmiSSLBase,      (ULONG) &ctx->amissl_base,
            AmiSSL_GetAmiSSLExtBase,   (ULONG) &ctx->amissl_ext_base,
            AmiSSL_SocketBase,         (ULONG) SocketBase,
            TAG_DONE);
        if (rc != 0 || ctx->amissl_base == NULL)
            goto fail_amissl_master;
        AmiSSLBase = ctx->amissl_base;
    }

    /* --- 3. SSL_CTX / SSL setup ------------------------------------------ */
    ctx->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (ctx->ssl_ctx == NULL)
        goto fail_amissl;

    if (insecure_skip_verify) {
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_verify(ctx->ssl_ctx,
                            SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                            NULL);
        if (!SSL_CTX_set_default_verify_paths(ctx->ssl_ctx))
            goto fail_ctx;
        /* Extra trust anchor for a private CA (issue #13) - added on top
         * of the AmiSSL: cert store above, not instead of it, so a broker
         * behind a normal public CA still verifies too. */
        if (ca_file != NULL &&
            !SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL))
            goto fail_ctx;
    }

    ctx->ssl = SSL_new(ctx->ssl_ctx);
    if (ctx->ssl == NULL)
        goto fail_ctx;

    SSL_set_tlsext_host_name(ctx->ssl, host);
    if (!insecure_skip_verify)
        SSL_set1_host(ctx->ssl, host);

    if (!SSL_set_fd(ctx->ssl, fd))
        goto fail_ssl;
    SSL_set_connect_state(ctx->ssl);

    /* --- 4. handshake, WaitSelect-gated (see amisslspike.c) -------------- */
    for (;;) {
        int rc;
        ERR_clear_error();
        rc = SSL_do_handshake(ctx->ssl);
        if (rc == 1)
            break;

        {
            int err = SSL_get_error(ctx->ssl, rc);
            if (err == SSL_ERROR_WANT_READ) {
                if (wait_gate(ctx, 0) < 0)
                    goto fail_ssl;
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                if (wait_gate(ctx, 1) < 0)
                    goto fail_ssl;
                continue;
            }
            goto fail_ssl; /* handshake failed outright */
        }
    }

    out->ctx = ctx;
    out->send = amissl_send;
    out->recv = amissl_recv;
    out->close = amissl_close;
    return 0;

fail_ssl:
    SSL_free(ctx->ssl);
    ctx->ssl = NULL;
fail_ctx:
    SSL_CTX_free(ctx->ssl_ctx);
    ctx->ssl_ctx = NULL;
fail_amissl:
    if (ctx->amissl_base != NULL) {
        CloseAmiSSL();
        ctx->amissl_base = NULL;
        ctx->amissl_ext_base = NULL;
    }
fail_amissl_master:
    CloseLibrary(AmiSSLMasterBase);
    ctx->amissl_master_base = NULL;
fail_fd:
    CloseSocket(fd);
    ctx->fd = -1;
fail_socket:
    CloseLibrary(SocketBase);
    ctx->socket_base = NULL;
    return -1;
}
