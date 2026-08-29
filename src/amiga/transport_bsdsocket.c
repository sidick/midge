/* transport_bsdsocket.c — AmigaOS bsdsocket.library mqtt_transport. See
 * /Users/simond/src/amiauth's src/amiga/sntp.c for the sibling pattern this
 * follows (per-task SocketBase, WaitSelect with SIGBREAKF_CTRL_C so network
 * waits stay abortable - CLAUDE.md). */
#include "transport_bsdsocket.h"

#include <exec/types.h>
#include <dos/dos.h> /* SIGBREAKF_CTRL_C */
#include <proto/exec.h>
#include <proto/socket.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <string.h>

/* WaitSelect() poll interval: short enough that mqtt_client_process()'s
 * caller gets a timely wakeup to check keepalive scheduling, matching the
 * host transport's SO_RCVTIMEO of 1s. */
#define MQTT_BSDSOCKET_POLL_SECS 1

/* proto/bsdsocket.h declares this `extern` (the bsdsocket clib2 headers
 * expect it to exist as a global, matching every AmigaOS bsdsocket client -
 * see amiauth's sntp.c) - must not be `static` here. */
struct Library *SocketBase = NULL;

static int bsdsocket_send(void *ctx, const uint8_t *buf, size_t len)
{
    bsdsocket_ctx *c = (bsdsocket_ctx *)ctx;
    long n = send(c->fd, (char *)buf, (long)len, 0);

    if (n < 0)
        return -1;
    return (int)n;
}

static int bsdsocket_recv(void *ctx, uint8_t *buf, size_t cap)
{
    bsdsocket_ctx *c = (bsdsocket_ctx *)ctx;
    struct timeval tv;
    fd_set rfds;
    ULONG sigmask = SIGBREAKF_CTRL_C;
    long n;

    FD_ZERO(&rfds);
    FD_SET(c->fd, &rfds);
    tv.tv_sec = MQTT_BSDSOCKET_POLL_SECS;
    tv.tv_usec = 0;

    n = WaitSelect(c->fd + 1, &rfds, NULL, NULL, &tv, &sigmask);
    if (n == 0)
        return 0; /* poll interval elapsed, nothing to read - not an error */
    if (n < 0) {
        if (sigmask & SIGBREAKF_CTRL_C)
            c->ctrl_c = 1;
        return -1;
    }

    n = recv(c->fd, (char *)buf, (long)cap, 0);
    if (n < 0)
        return -1;
    if (n == 0)
        return -1; /* peer closed the connection */
    return (int)n;
}

static void bsdsocket_close(void *ctx)
{
    bsdsocket_ctx *c = (bsdsocket_ctx *)ctx;

    if (c->fd >= 0) {
        CloseSocket(c->fd);
        c->fd = -1;
    }
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

int transport_bsdsocket_connect(mqtt_transport *out, bsdsocket_ctx *ctx,
                                 const char *host, uint16_t port)
{
    struct sockaddr_in addr;
    unsigned long ip;
    int fd;

    ctx->fd = -1;
    ctx->ctrl_c = 0;

    SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 4);
    if (!SocketBase)
        return -1; /* no TCP/IP stack */

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    ip = inet_addr((STRPTR)host);
    if (ip != (unsigned long)-1) {
        addr.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he || !he->h_addr_list[0]) {
            CloseLibrary(SocketBase);
            SocketBase = NULL;
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        CloseSocket(fd);
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        return -1;
    }

    ctx->fd = fd;
    out->ctx = ctx;
    out->send = bsdsocket_send;
    out->recv = bsdsocket_recv;
    out->close = bsdsocket_close;
    return 0;
}
