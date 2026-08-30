/* transport_openssl.c — TLS mqtt_transport for host builds, via OpenSSL
 * (1.1+/3.x API). Structurally mirrors transport_bsd.c: same TCP connect
 * loop and SO_RCVTIMEO poll trick, wrapped in an SSL object instead of a
 * bare socket. */

/* glibc hides struct addrinfo/getaddrinfo/freeaddrinfo (POSIX.1-2001) under
 * a strict -std=c99 build unless a feature-test macro says otherwise; must
 * be defined before the first system header (see transport_bsd.c). */
#define _POSIX_C_SOURCE 200112L

#include "transport_openssl.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>

static int openssl_send(void *ctx, const uint8_t *buf, size_t len)
{
    openssl_ctx *c = (openssl_ctx *)ctx;
    int n;

    ERR_clear_error();
    n = SSL_write(c->ssl, buf, (int)len);
    if (n <= 0) {
        int err = SSL_get_error(c->ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            return 0;
        return -1;
    }
    return n;
}

static int openssl_recv(void *ctx, uint8_t *buf, size_t cap)
{
    openssl_ctx *c = (openssl_ctx *)ctx;
    int n;

    ERR_clear_error();
    n = SSL_read(c->ssl, buf, (int)cap);
    if (n <= 0) {
        int err = SSL_get_error(c->ssl, n);

        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            return 0;
        if (err == SSL_ERROR_SYSCALL &&
            (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0; /* SO_RCVTIMEO's poll interval elapsed - see transport_bsd.c */
        return -1; /* SSL_ERROR_ZERO_RETURN (clean TLS shutdown) or fatal */
    }
    return n;
}

static void openssl_close(void *ctx)
{
    openssl_ctx *c = (openssl_ctx *)ctx;

    if (c->ssl != NULL) {
        SSL_shutdown(c->ssl); /* one call, best-effort - not a full bidirectional close */
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ssl_ctx != NULL) {
        SSL_CTX_free(c->ssl_ctx);
        c->ssl_ctx = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

/* Connects a blocking TCP socket to host:port. Returns the fd, or -1 on
 * failure. Duplicated from transport_bsd_connect rather than shared, to keep
 * transport_bsd.c untouched and this file self-contained. */
static int tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints, *res, *rp;
    char portstr[6];
    int fd = -1;
    struct timeval tv;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return -1;

    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

int transport_openssl_connect(mqtt_transport *out, openssl_ctx *ctx,
                               const char *host, uint16_t port,
                               int insecure_skip_verify,
                               const char *ca_file)
{
    int fd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;

    ctx->fd = -1;
    ctx->ssl_ctx = NULL;
    ctx->ssl = NULL;

    fd = tcp_connect(host, port);
    if (fd < 0)
        return -1;

    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (ssl_ctx == NULL) {
        fprintf(stderr, "mqtt: SSL_CTX_new failed\n");
        close(fd);
        return -1;
    }

    if (insecure_skip_verify) {
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_verify(ssl_ctx,
                            SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                            NULL);
        if (!SSL_CTX_set_default_verify_paths(ssl_ctx)) {
            fprintf(stderr, "mqtt: failed to load system trust store\n");
            SSL_CTX_free(ssl_ctx);
            close(fd);
            return -1;
        }
        /* Extra trust anchor for a private CA (issue #13) - added on top
         * of the system trust store above, not instead of it, so a broker
         * behind a normal public CA still verifies too. */
        if (ca_file != NULL &&
            !SSL_CTX_load_verify_locations(ssl_ctx, ca_file, NULL)) {
            fprintf(stderr, "mqtt: failed to load CA file %s\n", ca_file);
            SSL_CTX_free(ssl_ctx);
            close(fd);
            return -1;
        }
    }

    ssl = SSL_new(ssl_ctx);
    if (ssl == NULL) {
        fprintf(stderr, "mqtt: SSL_new failed\n");
        SSL_CTX_free(ssl_ctx);
        close(fd);
        return -1;
    }

    /* SNI, plus hostname verification when we're actually verifying - both
     * matter for brokers hosted behind name-based TLS termination. */
    SSL_set_tlsext_host_name(ssl, host);
    if (!insecure_skip_verify)
        SSL_set1_host(ssl, host);

    if (!SSL_set_fd(ssl, fd)) {
        fprintf(stderr, "mqtt: SSL_set_fd failed\n");
        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
        close(fd);
        return -1;
    }

    ERR_clear_error();
    if (SSL_connect(ssl) != 1) {
        /* SSL_get_verify_result() MUST be read before SSL_free() - after
         * free it silently reports X509_V_OK regardless of what actually
         * happened, which would misreport every handshake failure as a
         * verification success. */
        long verify_result = SSL_get_verify_result(ssl);

        fprintf(stderr, "mqtt: TLS handshake with %s:%u failed", host,
                (unsigned)port);
        if (verify_result != X509_V_OK)
            fprintf(stderr, " (%s)",
                    X509_verify_cert_error_string(verify_result));
        fprintf(stderr, "\n");

        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
        close(fd);
        return -1;
    }

    ctx->fd = fd;
    ctx->ssl_ctx = ssl_ctx;
    ctx->ssl = ssl;

    out->ctx = ctx;
    out->send = openssl_send;
    out->recv = openssl_recv;
    out->close = openssl_close;
    return 0;
}
