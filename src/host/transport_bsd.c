/* transport_bsd.c — BSD-socket mqtt_transport for host builds. */

/* glibc hides struct addrinfo/getaddrinfo/freeaddrinfo (POSIX.1-2001) under
 * a strict -std=c99 build unless a feature-test macro says otherwise; must
 * be defined before the first system header. macOS's libc exposes these
 * unconditionally, which is why this only broke in CI (Linux), not local
 * development on macOS - a lesson for any future host-only #include. */
#define _POSIX_C_SOURCE 200112L

#include "transport_bsd.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static int bsd_send(void *ctx, const uint8_t *buf, size_t len)
{
    bsd_ctx *c = (bsd_ctx *)ctx;
    ssize_t n;

    do {
        n = send(c->fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0)
        return -1;
    return (int)n;
}

static int bsd_recv(void *ctx, uint8_t *buf, size_t cap)
{
    bsd_ctx *c = (bsd_ctx *)ctx;
    ssize_t n;

    do {
        n = recv(c->fd, buf, cap, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        /* SO_RCVTIMEO's poll interval elapsed with nothing to read - not a
         * transport error, just "nothing right now" (see mqtt_transport.h). */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (n == 0)
        return -1; /* peer closed the connection */
    return (int)n;
}

static void bsd_close(void *ctx)
{
    bsd_ctx *c = (bsd_ctx *)ctx;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

int transport_bsd_connect(mqtt_transport *out, bsd_ctx *ctx,
                           const char *host, uint16_t port)
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

    ctx->fd = fd;
    out->ctx = ctx;
    out->send = bsd_send;
    out->recv = bsd_recv;
    out->close = bsd_close;
    return 0;
}
