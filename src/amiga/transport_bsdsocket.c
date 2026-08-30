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
#include <sys/ioctl.h> /* FIONBIO */
#include <errno.h> /* EINPROGRESS, via bsdsocket.library's Errno() below */

#include <string.h>

#include "tool_clock.h"

/* WaitSelect() poll interval: short enough that mqtt_client_process()'s
 * caller gets a timely wakeup to check keepalive scheduling, matching the
 * host transport's SO_RCVTIMEO of 1s. */
#define MQTT_BSDSOCKET_POLL_SECS 1

/* Overall connect-phase budget (issue #7) - same value and rationale as
 * transport_amissl.c's/transport_openssl.c's handshake timeouts: bounds a
 * host that accepts nothing (down, firewalled, wrong port) instead of
 * relying on the TCP stack's own connect timeout (often 75s+) with no way
 * to abort it in between. */
#define MQTT_BSDSOCKET_CONNECT_TIMEOUT_MS 30000u

/* proto/bsdsocket.h's macro-based inline stubs (inline/bsdsocket.h) call
 * through whatever C identifier named `SocketBase` is in scope at the call
 * site - they expand as plain text, e.g. `(struct Library *) (SocketBase)`,
 * not as a reference to a fixed extern. CLAUDE.md's static/extern trap
 * warns against a *file-scope* global here: mqtt.library (Phase 2) will
 * link this file into a single shared data segment used by every
 * connection subprocess, so one global SocketBase would make two
 * simultaneous connections trample each other's per-task library base
 * (bsdsocket handles are per-task - docs/ARCHITECTURE.md). Instead each
 * function below declares a function-local `SocketBase` that shadows the
 * `extern struct Library *SocketBase;` from proto/bsdsocket.h and is
 * initialised from the connection's own ctx->socket_base; the macros
 * resolve against that local, so every bsdsocket call here uses the right
 * connection's library base. Verified against
 * /opt/amiga/m68k-amigaos/ndk-include/inline/bsdsocket.h: BSDSOCKET_BASE_NAME
 * defaults to the bare identifier `SocketBase`, and each op (e.g.
 * WaitSelect) textually substitutes `(BSDSOCKET_BASE_NAME)` for the a6
 * register value - so the local shadow is what actually gets loaded into
 * a6, not the extern. */

static int bsdsocket_send(void *ctx, const uint8_t *buf, size_t len)
{
    bsdsocket_ctx *c = (bsdsocket_ctx *)ctx;
    struct Library *SocketBase = c->socket_base;
    long n = send(c->fd, (char *)buf, (long)len, 0);

    if (n < 0)
        return -1;
    return (int)n;
}

static int bsdsocket_recv(void *ctx, uint8_t *buf, size_t cap)
{
    bsdsocket_ctx *c = (bsdsocket_ctx *)ctx;
    struct Library *SocketBase = c->socket_base;
    struct timeval tv;
    fd_set rfds;
    ULONG sigmask = SIGBREAKF_CTRL_C | c->break_sigmask;
    long n;

    FD_ZERO(&rfds);
    FD_SET(c->fd, &rfds);
    tv.tv_sec = MQTT_BSDSOCKET_POLL_SECS;
    tv.tv_usec = 0;

    n = WaitSelect(c->fd + 1, &rfds, NULL, NULL, &tv, &sigmask);
    if (n == 0)
        return 0; /* poll interval elapsed, nothing to read - not an error */
    if (n < 0) {
        if (sigmask & SIGBREAKF_CTRL_C) {
            c->ctrl_c = 1;
            return -1;
        }
        if (sigmask & c->break_sigmask)
            /* Interrupted only by break_sigmask: the caller's own MsgPort
             * (or whatever break_sigmask names) has something for it -
             * treat this like a poll timeout so the caller's loop notices. */
            return 0;
        /* A genuine WaitSelect() failure (no break signal set at all, e.g.
         * the TCP stack died under us) - must not be reported as "no data",
         * or the pump busy-loops at 100% CPU forever. */
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
    struct Library *SocketBase = c->socket_base;

    if (c->fd >= 0) {
        CloseSocket(c->fd);
        c->fd = -1;
    }
    if (SocketBase) {
        CloseLibrary(SocketBase);
        c->socket_base = NULL;
    }
}

int transport_bsdsocket_connect(mqtt_transport *out, bsdsocket_ctx *ctx,
                                 const char *host, uint16_t port)
{
    struct Library *SocketBase;
    struct sockaddr_in addr;
    unsigned long ip;
    int fd;
    long one = 1, zero = 0;

    ctx->fd = -1;
    ctx->ctrl_c = 0;
    ctx->socket_base = NULL;
    ctx->break_sigmask = 0;

    SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 4);
    if (!SocketBase)
        return -1; /* no TCP/IP stack */
    ctx->socket_base = SocketBase;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    /* gethostbyname() itself has no WaitSelect()/CTRL_C abortability of
     * its own on this API - a DNS query against an unresponsive resolver
     * blocks for whatever the stack's own resolver timeout is (issue #7's
     * remaining gap; fixing it would need an async-resolve API this
     * classic BSD sockets surface doesn't offer). The connect() below is
     * the part that's actually fixed here. */
    ip = inet_addr((STRPTR)host);
    if (ip != (unsigned long)-1) {
        addr.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he || !he->h_addr_list[0])
            goto fail_socket_base;
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        goto fail_socket_base;

    /* Non-blocking for the connect attempt only (issue #7) - a host that
     * never responds (down, firewalled, wrong port) would otherwise block
     * here for the TCP stack's own connect timeout (often 75s+) with no
     * way to abort: unlike bsdsocket_recv()'s WaitSelect()-gated poll,
     * connect() on a blocking socket has no signal mask of its own to
     * pass SIGBREAKF_CTRL_C through. Reset to blocking again below once
     * connected - bsdsocket_send() still assumes that mode (a plain
     * blocking send(), no WaitSelect gating of its own). */
    if (IoctlSocket(fd, FIONBIO, (char *)&one) < 0)
        goto fail_fd;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        uint32_t connect_start;

        if (Errno() != EINPROGRESS)
            goto fail_fd;

        connect_start = tool_now_ms();
        for (;;) {
            struct timeval tv;
            fd_set wfds;
            ULONG sigmask = SIGBREAKF_CTRL_C;
            long n;

            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = MQTT_BSDSOCKET_POLL_SECS;
            tv.tv_usec = 0;

            n = WaitSelect(fd + 1, NULL, &wfds, NULL, &tv, &sigmask);
            if (n > 0)
                break; /* fd is writable - the attempt finished, either way */
            if (n < 0) {
                if (sigmask & SIGBREAKF_CTRL_C)
                    ctx->ctrl_c = 1;
                goto fail_fd;
            }
            if (tool_now_ms() - connect_start > MQTT_BSDSOCKET_CONNECT_TIMEOUT_MS)
                goto fail_fd;
        }

        /* This bsdsocket.library NDK has no SO_ERROR/SOL_SOCKET to ask
         * "did that connect actually succeed?" - getpeername() is the
         * portable substitute: it only succeeds on an established
         * connection, failing (typically ENOTCONN) if the peer refused or
         * the attempt otherwise failed. */
        {
            struct sockaddr_in peer;
            socklen_t peerlen = sizeof(peer);
            if (getpeername(fd, (struct sockaddr *)&peer, &peerlen) < 0)
                goto fail_fd;
        }
    }

    if (IoctlSocket(fd, FIONBIO, (char *)&zero) < 0)
        goto fail_fd;

    ctx->fd = fd;
    out->ctx = ctx;
    out->send = bsdsocket_send;
    out->recv = bsdsocket_recv;
    out->close = bsdsocket_close;
    return 0;

fail_fd:
    CloseSocket(fd);
fail_socket_base:
    CloseLibrary(SocketBase);
    ctx->socket_base = NULL;
    return -1;
}
