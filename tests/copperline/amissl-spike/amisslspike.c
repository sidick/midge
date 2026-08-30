/* amisslspike.c — de-risking SPIKE: does an AmiSSL TLS handshake driven by
 * SSL_set_fd() + a WaitSelect()-gated polling loop actually work on a
 * cross-compiled m68k/OS3 target, and does it round-trip a real MQTT
 * CONNECT/CONNACK exchange? This is throwaway investigative code (see
 * /Users/simond/src/midge/CLAUDE.md) — production polish is NOT the goal.
 * It is not built by the main midge build and is not linked into
 * mqtt.library or any shipped binary.
 *
 * Usage: amisslspike <ip> <port> [block|nonblock]   (default: nonblock)
 *
 *   nonblock (default) — the socket is set non-blocking via IoctlSocket()
 *     FIONBIO before SSL_set_fd(); SSL_do_handshake() then genuinely returns
 *     SSL_ERROR_WANT_READ/WANT_WRITE and the WaitSelect() gate in the retry
 *     loop below is what actually drives the handshake to completion. This
 *     is the shape transport_amissl.c (the real, future non-spike code)
 *     would use.
 *
 *   block — the socket is left blocking, reproducing the (abandoned) single
 *     blocking SSL_connect() path noted in micropython's modssl.c: on a
 *     blocking fd, SSL_do_handshake() mostly never surfaces WANT_READ
 *     because it blocks inside recv() internally, so the retry loop here
 *     will typically complete in one iteration with zero WANT_READ/
 *     WANT_WRITE hits. Kept so the trial runner can compare both shapes
 *     from the same binary.
 *
 * Progress and results go to both stdout (Printf) and the serial port (exec
 * RawPutChar, LVO -516) via out_str()/out_u32() — the Copperline harness
 * greps the serial stream. Grep for lines beginning "SPIKE: ".
 *
 * Result lines:
 *   SPIKE: mode=<block|nonblock>
 *   SPIKE: handshake-ok cipher=<name> proto=<name> iters=<n> want_read=<n> want_write=<n>
 *   SPIKE: roundtrip-ok
 *   SPIKE: PASS                                   (last line, only on full success)
 *   SPIKE: FAIL <stage> err=<n> ssl_err=<n>        (on any failure; exits nonzero)
 *
 * Exit code: 0 on SPIKE: PASS, nonzero otherwise (also matches on missing
 * "SPIKE: PASS" in the serial/stdout capture, belt and braces for the
 * harness).
 *
 * ---------------------------------------------------------------------
 * Library-base discipline (CLAUDE.md's binding rule even for a spike):
 *
 * SocketBase (bsdsocket.library) MUST NOT be a file-scope static/global —
 * see the big comment at the top of
 * /Users/simond/src/midge/src/amiga/transport_bsdsocket.c. proto/socket.h's
 * inline stubs expand against whatever C identifier named `SocketBase` is
 * in lexical scope, so a function-local `struct Library *SocketBase =
 * ctx->socket_base;` shadow is what actually gets used, per connection.
 * This spike is single-connection but follows the same discipline so the
 * pattern this program demonstrates is the one transport_amissl.c should
 * copy — every function below that calls a bsdsocket function declares its
 * own local SocketBase shadow.
 *
 * AmiSSLBase / AmiSSLExtBase / AmiSSLMasterBase are the one deliberate
 * exception: AmiSSL's v5 stub-link glue (libamisslstubs.a) is generated
 * against fixed global symbols named exactly AmiSSLBase/AmiSSLExtBase (see
 * amiauth's tests/copperline/amisslbench.c and micropython's
 * ports/amiga/amiga_ssl.c, both of which declare them file-scope for the
 * same reason). For a single-connection spike program this is acceptable.
 * The real transport_amissl.c must NOT do this — multiple simultaneous
 * mqtt.library connections would trample each other's AmiSSL base the same
 * way a global SocketBase would; that will need per-connection InitAmiSSL()
 * subprocess handling per the AmiSSL SDK's "Subprocesses" docs section, not
 * a repeat of this shortcut. */

#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/amisslmaster.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <proto/amisslmaster.h>
#include <amissl/tags.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>

#include <string.h>
#include <stdlib.h>

/* AmiSSL's stub-link glue requires these as fixed global symbols - see the
 * big comment above. Not a pattern to repeat in transport_amissl.c. */
struct Library *AmiSSLMasterBase;
struct Library *AmiSSLBase;
struct Library *AmiSSLExtBase;

/* Handshake/roundtrip retry budget - generous but finite so a stuck
 * WaitSelect() loop can never hang the harness forever. */
#define SPIKE_MAX_ITERS 120
#define SPIKE_POLL_SECS 1

/* Read buffer for the CONNACK: kept as a small static, not a stack array -
 * the Amiga shell stack is ~4KB by default (CLAUDE.md). This is a
 * single-instance spike program so a static is fine here; mqtt.library
 * proper must not do this (no statics that would break simultaneous
 * connections). */
static uint8_t g_connack[4];

/* --- serial + stdout dual output ------------------------------------- */

static void raw_put(char c)
{
    void *SysBase = *(void **)4UL;
    register long d0 __asm__("d0") = (unsigned char)c;
    register void *a6 __asm__("a6") = SysBase;
    __asm__ volatile("jsr -516(%%a6)" : : "r"(d0), "r"(a6)
                     : "d1", "a0", "a1", "cc", "memory");
}

static void out_str(const char *s)
{
    const char *p = s;
    while (*p)
        raw_put(*p++);
    Printf((STRPTR) "%s", (ULONG) s);
}

static void out_u32d(uint32_t v)
{
    char buf[16];
    int n = 0;
    uint32_t t = v;
    if (!t)
        buf[n++] = '0';
    while (t) {
        buf[n++] = (char)('0' + t % 10);
        t /= 10;
    }
    while (n)
        raw_put(buf[--n]);
    Printf((STRPTR) "%lu", (unsigned long) v);
}

static void out_nl(void)
{
    raw_put('\r');
    raw_put('\n');
    Printf((STRPTR) "\n");
}

static void fail(const char *stage, long err, unsigned long ssl_err)
{
    out_str("SPIKE: FAIL ");
    out_str(stage);
    out_str(" err=");
    out_u32d((uint32_t) err);
    out_str(" ssl_err=");
    out_u32d((uint32_t) ssl_err);
    out_nl();
}

/* --- WaitSelect gate, mirroring transport_bsdsocket.c's bsdsocket_recv --
 * shape exactly (1s tv, SIGBREAKF_CTRL_C in the mask, 0 = timeout-retry,
 * <0 with the ctrl-c bit set = abort). want_write selects which fd_set to
 * wait on. Returns 0 to keep retrying, -1 to abort (ctrl-C or a genuine
 * WaitSelect failure). */
static int wait_gate(struct Library *SocketBase, int fd, int want_write)
{
    struct timeval tv;
    fd_set fds;
    ULONG sigmask = SIGBREAKF_CTRL_C;
    long n;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = SPIKE_POLL_SECS;
    tv.tv_usec = 0;

    if (want_write)
        n = WaitSelect(fd + 1, NULL, &fds, NULL, &tv, &sigmask);
    else
        n = WaitSelect(fd + 1, &fds, NULL, NULL, &tv, &sigmask);

    if (n < 0) {
        if (sigmask & SIGBREAKF_CTRL_C)
            return -1; /* aborted */
        return -1; /* genuine WaitSelect failure - never spin at 100% CPU */
    }
    return 0; /* timeout or ready - either way, caller retries the SSL call */
}

/* --- minimal MQTT 3.1.1 CONNECT, hardcoded per the spike brief ---------
 * protocol name "MQTT", level 4, clean-session flag only, keepalive 60,
 * client id "spike" (5 bytes). Byte layout cross-checked against
 * mqtt_encode_connect() in /Users/simond/src/midge/src/core/mqtt_packet.c:
 *   fixed header:    0x10 (CONNECT<<4), remaining length 0x11 (17)
 *   variable header: 00 04 'M' 'Q' 'T' 'T'   (protocol name, len-prefixed)
 *                    04                       (protocol level 4 = 3.1.1)
 *                    02                       (connect flags: clean session)
 *                    00 3C                    (keepalive = 60s)
 *   payload:         00 05 's' 'p' 'i' 'k' 'e' (client id, len-prefixed)
 * 6 + 1 + 1 + 2 + 7 = 17 bytes of remaining length. Expected reply is a
 * CONNACK with no session-present flag and return code 0 (accepted). */
static const uint8_t g_connect_pkt[] = {
    0x10, 0x11,
    0x00, 0x04, 'M', 'Q', 'T', 'T',
    0x04,
    0x02,
    0x00, 0x3C,
    0x00, 0x05, 's', 'p', 'i', 'k', 'e'
};

int main(int argc, char **argv)
{
    struct Library *SocketBase;
    struct sockaddr_in addr;
    unsigned long ip;
    int fd = -1;
    int nonblock = 1;
    int rc_exit = 20;

    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;

    unsigned int want_read_count = 0, want_write_count = 0;
    int iters;

    if (argc < 3) {
        out_str("SPIKE: FAIL usage err=0 ssl_err=0");
        out_nl();
        out_str("usage: amisslspike <ip> <port> [block|nonblock]");
        out_nl();
        return 2;
    }
    if (argc >= 4 && strcmp(argv[3], "block") == 0)
        nonblock = 0;

    out_str("SPIKE: mode=");
    out_str(nonblock ? "nonblock" : "block");
    out_nl();

    /* --- 1. bsdsocket.library, socket(), connect() ---------------------- */
    SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 4);
    if (!SocketBase) {
        fail("open-bsdsocket", -1, 0);
        return 10;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short) atoi(argv[2]));
    ip = inet_addr((STRPTR) argv[1]);
    if (ip == (unsigned long) -1) {
        fail("parse-ip", -1, 0);
        CloseLibrary(SocketBase);
        return 10;
    }
    addr.sin_addr.s_addr = ip;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket", Errno(), 0);
        CloseLibrary(SocketBase);
        return 10;
    }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fail("connect", Errno(), 0);
        CloseSocket(fd);
        CloseLibrary(SocketBase);
        return 10;
    }

    if (nonblock) {
        long one = 1;
        if (IoctlSocket(fd, FIONBIO, (char *) &one) < 0) {
            fail("ioctlsocket-fionbio", Errno(), 0);
            CloseSocket(fd);
            CloseLibrary(SocketBase);
            return 10;
        }
    }

    /* --- 3. amisslmaster.library + OpenAmiSSLTags ------------------------
     * AmiSSL_SocketBase is passed (unlike amisslbench.c, which is
     * network-free): this spike does real socket I/O through AmiSSL.
     *
     * InitAmiSSL() (called internally by OpenAmiSSLTags()) reads its cert
     * store/config via the AmiSSL: assign even with SSL_VERIFY_NONE (see
     * amiauth's amissl-bench.sh comment). The boot volume is now a real
     * amibake-built AmigaOS 3.2.2 image (~/src/amibake, manifests/
     * AmiSSLSpike.toml) with the amissl recipe's own install layout -
     * Devs/AmiSSL/Certs, per its [install].assigns declaring AmiSSL: ->
     * SYS:Devs/AmiSSL - so the assign is done programmatically here rather
     * than relying on amibake to have injected it into Startup-Sequence
     * (it doesn't yet). DOSBase here is the auto-opened global from crt0
     * (a standalone CLI program, not mqtt.library's multi-connection
     * shared-library case the SocketBase shadow discipline above exists
     * for - no per-connection concern). */
    {
        BPTR lock = Lock((STRPTR) "AmiSSLSpike:Devs/AmiSSL", ACCESS_READ);
        if (!lock) {
            fail("assign-amissl", IoErr(), 0);
            CloseSocket(fd);
            CloseLibrary(SocketBase);
            return 11;
        }
        if (!AssignLock((STRPTR) "AmiSSL", lock)) {
            fail("assign-amissl", IoErr(), 0);
            UnLock(lock);
            CloseSocket(fd);
            CloseLibrary(SocketBase);
            return 11;
        }
    }

    AmiSSLMasterBase = OpenLibrary((STRPTR) "amisslmaster.library", 5);
    if (!AmiSSLMasterBase) {
        fail("open-amisslmaster", -1, 0);
        CloseSocket(fd);
        CloseLibrary(SocketBase);
        return 11;
    }

    {
        LONG rc;
        LONG amissl_errno = 0;
        rc = OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
            AmiSSL_UsesOpenSSLStructs, FALSE,
            AmiSSL_GetAmiSSLBase,      (ULONG) &AmiSSLBase,
            AmiSSL_GetAmiSSLExtBase,   (ULONG) &AmiSSLExtBase,
            AmiSSL_SocketBase,         (ULONG) SocketBase,
            AmiSSL_ErrNoPtr,           (ULONG) &amissl_errno,
            TAG_DONE);
        if (rc != 0 || AmiSSLBase == NULL) {
            fail("open-amissl-tags", rc, (unsigned long) amissl_errno);
            CloseLibrary(AmiSSLMasterBase);
            CloseSocket(fd);
            CloseLibrary(SocketBase);
            return 11;
        }
    }

    /* --- 4. SSL_CTX / SSL setup ------------------------------------------
     * SSL_VERIFY_NONE deliberately: the spike question is handshake I/O
     * reliability against a self-signed test broker, not the trust store. */
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        fail("ssl-ctx-new", 0, ERR_get_error());
        goto cleanup_amissl;
    }
    SSL_CTX_set_security_level(ctx, 0);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    ssl = SSL_new(ctx);
    if (!ssl) {
        fail("ssl-new", 0, ERR_get_error());
        goto cleanup_ctx;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);

    /* --- 5. handshake loop ------------------------------------------------ */
    for (iters = 0; iters < SPIKE_MAX_ITERS; iters++) {
        int rc;
        ERR_clear_error();
        rc = SSL_do_handshake(ssl);
        if (rc == 1)
            break;

        {
            int err = SSL_get_error(ssl, rc);
            if (err == SSL_ERROR_WANT_READ) {
                want_read_count++;
                if (wait_gate(SocketBase, fd, 0) < 0) {
                    fail("handshake-wait-read", -1, (unsigned long) err);
                    goto cleanup_ssl;
                }
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                want_write_count++;
                if (wait_gate(SocketBase, fd, 1) < 0) {
                    fail("handshake-wait-write", -1, (unsigned long) err);
                    goto cleanup_ssl;
                }
                continue;
            }
            fail("handshake", (long) err, ERR_get_error());
            goto cleanup_ssl;
        }
    }
    if (iters >= SPIKE_MAX_ITERS) {
        fail("handshake-budget-exceeded", (long) iters, 0);
        goto cleanup_ssl;
    }

    /* --- 6. report negotiated cipher/protocol ----------------------------- */
    out_str("SPIKE: handshake-ok cipher=");
    out_str(SSL_get_cipher(ssl));
    out_str(" proto=");
    out_str(SSL_get_version(ssl));
    out_str(" iters=");
    out_u32d((uint32_t) iters);
    out_str(" want_read=");
    out_u32d(want_read_count);
    out_str(" want_write=");
    out_u32d(want_write_count);
    out_nl();

    /* --- 7. round-trip proof: MQTT CONNECT / CONNACK ----------------------- */
    {
        size_t written = 0;
        int budget;

        for (budget = 0; budget < SPIKE_MAX_ITERS && written < sizeof(g_connect_pkt); budget++) {
            int rc;
            ERR_clear_error();
            rc = SSL_write(ssl, g_connect_pkt + written, (int) (sizeof(g_connect_pkt) - written));
            if (rc > 0) {
                written += (size_t) rc;
                continue;
            }
            {
                int err = SSL_get_error(ssl, rc);
                if (err == SSL_ERROR_WANT_READ) {
                    want_read_count++;
                    if (wait_gate(SocketBase, fd, 0) < 0) {
                        fail("connect-write-wait-read", -1, (unsigned long) err);
                        goto cleanup_ssl;
                    }
                    continue;
                }
                if (err == SSL_ERROR_WANT_WRITE) {
                    want_write_count++;
                    if (wait_gate(SocketBase, fd, 1) < 0) {
                        fail("connect-write-wait-write", -1, (unsigned long) err);
                        goto cleanup_ssl;
                    }
                    continue;
                }
                fail("connect-write", (long) err, ERR_get_error());
                goto cleanup_ssl;
            }
        }
        if (written < sizeof(g_connect_pkt)) {
            fail("connect-write-budget-exceeded", (long) budget, 0);
            goto cleanup_ssl;
        }

        {
            size_t got = 0;
            for (budget = 0; budget < SPIKE_MAX_ITERS && got < sizeof(g_connack); budget++) {
                int rc;
                ERR_clear_error();
                rc = SSL_read(ssl, g_connack + got, (int) (sizeof(g_connack) - got));
                if (rc > 0) {
                    got += (size_t) rc;
                    continue;
                }
                {
                    int err = SSL_get_error(ssl, rc);
                    if (err == SSL_ERROR_WANT_READ) {
                        want_read_count++;
                        if (wait_gate(SocketBase, fd, 0) < 0) {
                            fail("connack-read-wait-read", -1, (unsigned long) err);
                            goto cleanup_ssl;
                        }
                        continue;
                    }
                    if (err == SSL_ERROR_WANT_WRITE) {
                        want_write_count++;
                        if (wait_gate(SocketBase, fd, 1) < 0) {
                            fail("connack-read-wait-write", -1, (unsigned long) err);
                            goto cleanup_ssl;
                        }
                        continue;
                    }
                    fail("connack-read", (long) err, ERR_get_error());
                    goto cleanup_ssl;
                }
            }
            if (got < sizeof(g_connack)) {
                fail("connack-read-budget-exceeded", (long) budget, 0);
                goto cleanup_ssl;
            }
        }

        if (g_connack[0] != 0x20 || g_connack[1] != 0x02 ||
            g_connack[2] != 0x00 || g_connack[3] != 0x00) {
            out_str("SPIKE: FAIL connack-mismatch got=");
            out_u32d(g_connack[0]);
            out_str(",");
            out_u32d(g_connack[1]);
            out_str(",");
            out_u32d(g_connack[2]);
            out_str(",");
            out_u32d(g_connack[3]);
            out_nl();
            goto cleanup_ssl;
        }
    }

    out_str("SPIKE: roundtrip-ok");
    out_nl();
    out_str("SPIKE: PASS");
    out_nl();
    rc_exit = 0;

cleanup_ssl:
    /* One SSL_shutdown() call, result ignored, per the brief - a full
     * bidirectional close-notify handshake isn't needed for a spike. Read
     * verify_result before SSL_free() if ever needed - not used here since
     * verification is off, noted per the micropython modssl.c pitfall. */
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
cleanup_ctx:
    if (ctx)
        SSL_CTX_free(ctx);
cleanup_amissl:
    if (AmiSSLBase) {
        CloseAmiSSL();
        AmiSSLBase = NULL;
        AmiSSLExtBase = NULL;
    }
    if (AmiSSLMasterBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
    if (fd >= 0)
        CloseSocket(fd);
    CloseLibrary(SocketBase);

    return rc_exit;
}
