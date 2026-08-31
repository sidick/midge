/* mqttstats_main.c - mqttstats: a headless AmigaOS Commodity publishing
 * Amiga system telemetry (uptime, chip/fast RAM free, CPU model) to a
 * Home Assistant broker via mqtt.library, with full MQTT Discovery so the
 * sensors appear in HA automatically (issue #6's telemetry piece - I2C
 * sensor support is deliberately deferred to a follow-up, see that issue).
 *
 * Design goals, all from the issue discussion:
 *   - Configurable as much as possible from a Workbench icon's ToolTypes -
 *     amiga.lib's ArgArrayInit()/ArgString()/ArgInt() read WBStartup
 *     ToolTypes and Shell KEYWORD=VALUE arguments through the exact same
 *     calls (ArgArrayInit() detects argc==0, the Workbench-launch case,
 *     and pulls ToolTypes from the WBStartup message libnix's crt0
 *     stashes for it - no icon.library calls needed here at all).
 *   - Silent: no window, ever. Diagnostics go to the serial port only (see
 *     raw_str() below), never stdio: a libnix program's first stdio write
 *     auto-opens a console window when there's no controlling CLI (e.g.
 *     a Workbench/WBStartup launch) - on-target testing confirmed this
 *     does NOT block the calling Shell/Startup-Sequence (an earlier
 *     working theory to that effect didn't hold up under a proper
 *     isolated repro), but a popped-up window is still a visible UI,
 *     which this design goal rules out regardless of whether anything
 *     blocks on it.
 *   - A commodities.library broker so Commodities Exchange can Enable/
 *     Disable/Kill it - the only control surface this tool has, matching
 *     "no UI, just disable and quit" from the issue. Unlike a typical
 *     Commodity there's no HotKey/AttachCxObj: this program never filters
 *     input events, so the broker exists purely for that Exchange control
 *     surface, not to intercept anything.
 *
 * See src/tools/ha_discovery.h for the (portable, host-tested) Home
 * Assistant MQTT Discovery topic/payload format this publishes.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <libraries/commodities.h>
#include <libraries/mqtt.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/commodities.h>
#include <proto/mqtt.h>
#include <proto/timer.h>
#include <proto/intuition.h>

#include <clib/alib_protos.h> /* ArgArrayInit/ArgArrayDone/ArgString/ArgInt -
                                  amiga.lib, not an LVO library: no proto
                                  stub header of its own, just prototypes */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ha_discovery.h"
#include "version.h"

MIDGE_VERSTAG("mqttstats")

/* No stdio for diagnostics (fprintf/printf), deliberately: this program
 * must never pop a console window under any circumstance (see this
 * file's own "Silent" design goal) - a libnix program's first stdio
 * write auto-opens one when there's no controlling CLI, and that's a
 * visible UI this design goal rules out regardless of whether anything
 * ever blocks on it (on-target testing found it doesn't). Same
 * exec/RawPutChar (LVO -516) serial-only diagnostic path already used by
 * tests/copperline and tests/library test tools - visible under Copperline
 * or a real serial cable, otherwise simply goes nowhere, never a window. */
static void raw_put(char c)
{
    void *SysBase = *(void **)4UL;
    register long d0 __asm__("d0") = (unsigned char)c;
    register void *a6 __asm__("a6") = SysBase;
    __asm__ volatile("jsr -516(%%a6)" : : "r"(d0), "r"(a6)
                     : "d1", "a0", "a1", "cc", "memory");
}

static void raw_str(const char *s)
{
    while (*s)
        raw_put(*s++);
}

/* Every mqtt.library caller must define this global itself - see
 * examples/pubexample.c's banner for why (no libnix auto-open for a
 * library the linker doesn't know about). This program is a single,
 * non-library, singleton executable (enforced by NBU_UNIQUE below), so a
 * plain global is fine here - not the multi-connection subprocess
 * situation CLAUDE.md's static/extern trap warns about inside
 * mqtt.library itself. */
struct Library *MqttBase;
struct Library *CxBase;
struct IntuitionBase *IntuitionBase; /* opened only for the connect-refused
                                         alert below - see
                                         alert_connect_refused(). proto/
                                         intuition.h itself declares this
                                         extern (typed struct IntuitionBase
                                         *, unlike the plain struct Library
                                         * every other base here uses) -
                                         this is the matching definition,
                                         not a fresh global. */
struct Device *TimerBase;
struct Library *WorkbenchBase; /* version only - see workbench_version() */

/* Poll tick for the main loop - see run_loop()'s own comment for why a
 * plain Delay()-based poll was chosen over multiplexing a WaitSelect()-
 * style signal mask. */
#define MQTTSTATS_POLL_TICKS 50 /* ~1s @ 50Hz */

#define MQTTSTATS_TOPIC_BUF 128
#define MQTTSTATS_PAYLOAD_BUF 512
#define MQTTSTATS_VALUE_BUF 32

/* --- ToolTypes-derived configuration --------------------------------- */

typedef struct {
    STRPTR host;
    UWORD port;
    STRPTR client_id;
    STRPTR node_id;    /* sanitized into node_id_buf if not given explicitly */
    STRPTR device_name;
    STRPTR username;
    STRPTR password;
    BOOL tls;
    BOOL tls_insecure;
    STRPTR ca_file;
    ULONG interval_secs;
    BYTE cx_priority;
} mqttstats_config;

/* MQTT topics/HA unique_ids only tolerate a conservative character set -
 * copy `src` into `dst` (cap bytes) replacing anything outside
 * [A-Za-z0-9_-] with '_'. Truncates rather than overflowing; a truncated
 * node id is still unique enough in practice (CLIENTID collisions this
 * would create are a pre-existing user misconfiguration, not something
 * this function needs to detect). */
static void sanitize_node_id(const char *src, char *dst, size_t cap)
{
    size_t i = 0;

    if (cap == 0)
        return;
    cap--;
    for (; *src && i < cap; src++) {
        char c = *src;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        dst[i++] = ok ? c : '_';
    }
    dst[i] = '\0';
}

static void read_config(STRPTR *tt, mqttstats_config *cfg,
                         char *node_id_buf, size_t node_id_cap)
{
    STRPTR explicit_node_id;

    memset(cfg, 0, sizeof(*cfg));
    cfg->host = ArgString((CONST_STRPTR *)tt, (CONST_STRPTR) "HOST", NULL);
    cfg->port = (UWORD) ArgInt((CONST_STRPTR *)tt, (CONST_STRPTR) "PORT", 0);
    cfg->client_id = ArgString((CONST_STRPTR *)tt, (CONST_STRPTR) "CLIENTID",
                                (CONST_STRPTR) "midge-stats");
    cfg->device_name = ArgString((CONST_STRPTR *)tt, (CONST_STRPTR) "DEVICENAME",
                                  (CONST_STRPTR) "Amiga");
    cfg->username = ArgString((CONST_STRPTR *)tt, (CONST_STRPTR) "USER", NULL);
    cfg->password = ArgString((CONST_STRPTR *)tt, (CONST_STRPTR) "PASSWORD", NULL);
    cfg->tls = ArgInt((CONST_STRPTR *)tt, (CONST_STRPTR) "TLS", 0) ? TRUE : FALSE;
    cfg->tls_insecure = ArgInt((CONST_STRPTR *)tt, (CONST_STRPTR) "TLSINSECURE", 0) ? TRUE : FALSE;
    if (cfg->tls_insecure)
        cfg->tls = TRUE;
    cfg->ca_file = ArgString((CONST_STRPTR *)tt, (CONST_STRPTR) "CAFILE", NULL);
    cfg->interval_secs = (ULONG) ArgInt((CONST_STRPTR *)tt, (CONST_STRPTR) "INTERVAL", 60);
    if (cfg->interval_secs == 0)
        cfg->interval_secs = 60;
    cfg->cx_priority = (BYTE) ArgInt((CONST_STRPTR *)tt, (CONST_STRPTR) "CX_PRIORITY", 0);

    if (cfg->port == 0)
        cfg->port = cfg->tls ? 8883 : 1883;

    explicit_node_id = ArgString((CONST_STRPTR *)tt, (CONST_STRPTR) "NODEID", NULL);
    sanitize_node_id((const char *)(explicit_node_id ? explicit_node_id
                                                       : cfg->client_id),
                      node_id_buf, node_id_cap);
    cfg->node_id = (STRPTR) node_id_buf;
}

/* --- Telemetry gathering ------------------------------------------------ */

/* An earlier version of this file used timer.device's TR_GETSYSTIME here,
 * on the mistaken premise that it reports elapsed time since the last
 * system reset. It doesn't: TR_GETSYSTIME returns the *system clock* -
 * the same absolute, calendar-rooted clock dos.library's DateStamp() is
 * built on (seconds since 1978-01-01, set from the battery-backed RTC at
 * boot if one's fitted) - just with sub-second precision. Reported as
 * "uptime" it produced a nonsensical near-50-year value on any machine
 * with a working clock; caught in real use, not in testing. What's
 * actually wanted is `ReadEClock()`'s free-running hardware tick counter
 * (see uptime_secs() below): it genuinely does reset to 0 at power-on,
 * has no notion of calendar date at all, and is exactly as immune to the
 * DateStamp() clock-jump hazard issue #8 documents (a live SetClock or
 * timezone change can't move it) - it just doesn't ALSO track wall time,
 * which this program never needed anyway. Used for both the reported
 * "uptime" metric and this program's own interval scheduling. This
 * program is a standalone singleton (NBU_UNIQUE enforces at most one
 * instance), so opening the device once at startup and keeping it for
 * the process's lifetime is completely safe - there is no second
 * instance to trample it. */
static struct timerequest *g_timerreq;
static struct MsgPort *g_timerport;

static int open_timer(void)
{
    g_timerport = CreateMsgPort();
    if (!g_timerport)
        return -1;
    g_timerreq = (struct timerequest *)
        CreateIORequest(g_timerport, sizeof(*g_timerreq));
    if (!g_timerreq) {
        DeleteMsgPort(g_timerport);
        g_timerport = NULL;
        return -1;
    }
    if (OpenDevice((STRPTR) TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)g_timerreq, 0) != 0) {
        DeleteIORequest((struct IORequest *)g_timerreq);
        DeleteMsgPort(g_timerport);
        g_timerreq = NULL;
        g_timerport = NULL;
        return -1;
    }
    TimerBase = g_timerreq->tr_node.io_Device;
    return 0;
}

static void close_timer(void)
{
    if (TimerBase) {
        CloseDevice((struct IORequest *)g_timerreq);
        TimerBase = NULL;
    }
    if (g_timerreq) {
        DeleteIORequest((struct IORequest *)g_timerreq);
        g_timerreq = NULL;
    }
    if (g_timerport) {
        DeleteMsgPort(g_timerport);
        g_timerport = NULL;
    }
}

/* Seconds since the last system reset - see this section's own banner
 * comment for why this is ReadEClock(), not TR_GETSYSTIME. ReadEClock()
 * is a direct library call through TimerBase (set by open_timer()'s
 * OpenDevice(), any unit), not an IORequest - no DoIO() here. */
static ULONG uptime_secs(void)
{
    struct EClockVal ec;
    ULONG freq = ReadEClock(&ec);
    unsigned long long ticks = ((unsigned long long) ec.ev_hi << 32) | ec.ev_lo;

    return freq ? (ULONG) (ticks / freq) : 0;
}

static const char *cpu_model(void)
{
    struct ExecBase *eb = (struct ExecBase *) SysBase;

    if (eb->AttnFlags & AFF_68060)
        return "68060";
    if (eb->AttnFlags & AFF_68040)
        return "68040";
    if (eb->AttnFlags & AFF_68030)
        return "68030";
    if (eb->AttnFlags & AFF_68020)
        return "68020";
    return "68000";
}

/* format_bcd_version() below works around a confirmed m68k-amigaos-gcc -O2
 * miscompile (not an AmigaOS/NDK issue): reading a UWORD struct field
 * (lib_Version/lib_Revision) into an sprintf() "%u" call leaves garbage in
 * the promoted value's upper 16 bits - confirmed via an isolated repro,
 * -O0 unaffected. Neither an explicit `& 0xFFFF` mask (optimized away as
 * "provably redundant" by GCC's own - wrong, here - UWORD range analysis;
 * confirmed by a byte-identical rebuild with and without it) nor routing
 * through a `volatile` local (works in an isolated repro but NOT in this
 * file's actual build - confirmed on-target, still wrong here) reliably
 * fixed it, which means whatever's really going on is sensitive to this
 * file's surrounding code, not just the read+format pattern in isolation.
 * Sidestepping sprintf's "%u" varargs path entirely with a plain,
 * never-inlined, non-variadic formatter is the one approach confirmed
 * correct on-target for these two call sites - see the version-history
 * around this comment (git blame) if a future toolchain upgrade makes it
 * worth revisiting whether this workaround is still needed. */
static void __attribute__((noinline))
format_bcd_version(char *buf, unsigned long ver, unsigned long rev)
{
    unsigned long vals[2];
    int vi;
    char *p = buf;

    vals[0] = ver;
    vals[1] = rev;
    for (vi = 0; vi < 2; vi++) {
        char digits[6];
        int n = 0;
        unsigned long v = vals[vi];

        if (vi == 1)
            *p++ = '.';
        if (v == 0) {
            digits[n++] = '0';
        } else {
            while (v > 0 && n < (int) sizeof(digits)) {
                digits[n++] = (char) ('0' + (v % 10));
                v /= 10;
            }
        }
        while (n > 0)
            *p++ = digits[--n];
    }
    *p = '\0';
}

/* Kickstart's own version is just exec.library's - SysBase is always open
 * (it's how every program gets here at all), so unlike workbench_version()
 * below there's no "library missing" case to handle. */
static void kickstart_version(char *buf)
{
    struct ExecBase *eb = (struct ExecBase *) SysBase;
    volatile unsigned long ver = eb->LibNode.lib_Version;
    volatile unsigned long rev = eb->LibNode.lib_Revision;

    format_bcd_version(buf, ver, rev);
}

/* Kickstart (ROM) and Workbench (disk-based) versions can genuinely differ
 * - e.g. a newer ROM booted against an older Workbench disk set, or vice
 * versa - so this is deliberately a separate sensor from kickstart_version
 * above, not assumed to always match it. workbench.library's own version
 * tracks the installed Workbench release the same way exec.library's
 * tracks Kickstart. WorkbenchBase is opened once in main() and may be NULL
 * (a bare CLI-only setup that's never actually installed/loaded Workbench,
 * however rare) - reported as "unavailable" rather than a misleading 0.0. */
static void workbench_version(char *buf)
{
    if (WorkbenchBase) {
        volatile unsigned long ver = WorkbenchBase->lib_Version;
        volatile unsigned long rev = WorkbenchBase->lib_Revision;

        format_bcd_version(buf, ver, rev);
    } else {
        strcpy(buf, "unavailable");
    }
}

/* --- Publishing ---------------------------------------------------------- */

static int publish_retained(APTR client, const char *topic, const char *value,
                             UBYTE qos)
{
    return MQTT_Publish(client, (STRPTR)(char *)topic, (APTR)(char *)value,
                         (ULONG) strlen(value), TRUE, qos) == 0 ? 0 : -1;
}

static void publish_discovery(APTR client, const ha_device *dev)
{
    char topic[MQTTSTATS_TOPIC_BUF];
    char payload[MQTTSTATS_PAYLOAD_BUF];

    if (ha_discovery_topic(dev, "uptime", topic, sizeof(topic)) > 0 &&
        ha_discovery_payload(dev, "uptime", "Uptime", "s", "duration",
                              payload, sizeof(payload)) > 0)
        publish_retained(client, topic, payload, 1);

    if (ha_discovery_topic(dev, "chip_free", topic, sizeof(topic)) > 0 &&
        ha_discovery_payload(dev, "chip_free", "Chip RAM Free", "B",
                              "data_size", payload, sizeof(payload)) > 0)
        publish_retained(client, topic, payload, 1);

    if (ha_discovery_topic(dev, "fast_free", topic, sizeof(topic)) > 0 &&
        ha_discovery_payload(dev, "fast_free", "Fast RAM Free", "B",
                              "data_size", payload, sizeof(payload)) > 0)
        publish_retained(client, topic, payload, 1);

    if (ha_discovery_topic(dev, "cpu_model", topic, sizeof(topic)) > 0 &&
        ha_discovery_payload(dev, "cpu_model", "CPU Model", NULL, NULL,
                              payload, sizeof(payload)) > 0)
        publish_retained(client, topic, payload, 1);

    if (ha_discovery_topic(dev, "kickstart_version", topic, sizeof(topic)) > 0 &&
        ha_discovery_payload(dev, "kickstart_version", "Kickstart Version",
                              NULL, NULL, payload, sizeof(payload)) > 0)
        publish_retained(client, topic, payload, 1);

    if (ha_discovery_topic(dev, "workbench_version", topic, sizeof(topic)) > 0 &&
        ha_discovery_payload(dev, "workbench_version", "Workbench Version",
                              NULL, NULL, payload, sizeof(payload)) > 0)
        publish_retained(client, topic, payload, 1);
}

static void publish_availability(APTR client, const ha_device *dev,
                                  const char *state)
{
    char topic[MQTTSTATS_TOPIC_BUF];

    if (ha_availability_topic(dev, topic, sizeof(topic)) > 0)
        publish_retained(client, topic, state, 0);
}

static void publish_state(APTR client, const ha_device *dev)
{
    char topic[MQTTSTATS_TOPIC_BUF];
    char value[MQTTSTATS_VALUE_BUF];

    if (ha_state_topic(dev, "uptime", topic, sizeof(topic)) > 0) {
        sprintf(value, "%lu", (unsigned long) uptime_secs());
        publish_retained(client, topic, value, 0);
    }
    if (ha_state_topic(dev, "chip_free", topic, sizeof(topic)) > 0) {
        sprintf(value, "%lu", (unsigned long) AvailMem(MEMF_CHIP));
        publish_retained(client, topic, value, 0);
    }
    if (ha_state_topic(dev, "fast_free", topic, sizeof(topic)) > 0) {
        sprintf(value, "%lu", (unsigned long) AvailMem(MEMF_FAST));
        publish_retained(client, topic, value, 0);
    }
    if (ha_state_topic(dev, "cpu_model", topic, sizeof(topic)) > 0)
        publish_retained(client, topic, cpu_model(), 0);
    if (ha_state_topic(dev, "kickstart_version", topic, sizeof(topic)) > 0) {
        kickstart_version(value);
        publish_retained(client, topic, value, 0);
    }
    if (ha_state_topic(dev, "workbench_version", topic, sizeof(topic)) > 0) {
        workbench_version(value);
        publish_retained(client, topic, value, 0);
    }
}

/* --- Initial-connect retry ------------------------------------------------
 *
 * mco_AutoReconnect (see MQTT_CreateClient()'s opts) only covers a drop
 * *after* MQTT_Connect() has already succeeded once - it explicitly does
 * not retry a failed first attempt (see <libraries/mqtt.h>'s own comment on
 * mco_AutoReconnect). That matters here specifically because mqttstats is
 * meant to run from WBStartup, where there's no guaranteed ordering against
 * the TCP/IP stack's own WBStartup entry (Roadshow/AmiTCP/Miami) - launched
 * before bsdsocket.library is up, a bare single MQTT_Connect() attempt
 * would just fail once and quit for the rest of the session. So this loop
 * retries MQTT_Connect() itself, with the same capped exponential backoff
 * mco_AutoReconnect uses internally (1s, 2s, 4s, ... capped at 32s,
 * forever) - EXCEPT for MQTT_CONNECT_REFUSED (see alert_connect_refused()
 * below), which means the broker is up and actively saying no: retrying
 * that unchanged would just spam it forever for no reason, so it stops and
 * surfaces the problem instead. */

/* Polls cxport/CTRL_C in MQTTSTATS_POLL_TICKS chunks across a backoff wait,
 * so a Kill from Exchange (or Ctrl-C from a Shell launch) can interrupt a
 * long wait instead of only being noticed once run_loop() itself starts.
 * ENABLE/DISABLE while still trying to connect have nothing to toggle yet
 * (no client, no availability topic to publish), so are drained and
 * ignored here - Exchange's Enable/Disable simply has no effect until the
 * initial connect succeeds. Returns TRUE if the caller should give up
 * (killed/interrupted), FALSE if the wait completed normally. */
static BOOL wait_ticks_or_kill(struct MsgPort *cxport, ULONG total_ticks)
{
    ULONG waited = 0;

    while (waited < total_ticks) {
        if (cxport) {
            CxMsg *cxm;
            while ((cxm = (CxMsg *) GetMsg(cxport)) != NULL) {
                ULONG type = CxMsgType(cxm);
                ULONG id = CxMsgID(cxm);
                ReplyMsg((struct Message *) cxm);
                if (type == CXM_COMMAND && id == CXCMD_KILL)
                    return TRUE;
            }
        }
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            return TRUE;
        }
        Delay(MQTTSTATS_POLL_TICKS);
        waited += MQTTSTATS_POLL_TICKS;
    }
    return FALSE;
}

/* MQTT_CONNECT_REFUSED means a config problem (bad credentials, rejected
 * client id, ...) that silently retrying forever will never fix - only
 * relaunching after editing the icon's ToolTypes will. Left to just quit
 * quietly (the original design, see this file's own history), a broker
 * that requires auth but was never given any would fail every single
 * launch with no visible indication why. mqtt.library itself has no idea
 * this alert exists or should happen - it only ever returns the plain
 * negative code above; opening intuition.library and deciding to show
 * something is entirely this application's own choice, same as any other
 * mqtt.library caller is free to make differently. */
static void alert_connect_refused(const char *host)
{
    struct EasyStruct es;

    raw_str("mqttstats: broker refused the connection (bad credentials, "
            "client id, or protocol version?)\r\n");

    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((STRPTR) "intuition.library", 37);
    if (!IntuitionBase)
        return;

    es.es_StructSize = sizeof(es);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *) "mqttstats";
    es.es_TextFormat = (UBYTE *)
        "mqttstats: %s refused the connection.\n"
        "Check HOST/USER/PASSWORD/CLIENTID in this icon's Tool Types, "
        "then relaunch.";
    es.es_GadgetFormat = (UBYTE *) "OK";
    EasyRequest(NULL, &es, NULL, host);

    CloseLibrary((struct Library *) IntuitionBase);
    IntuitionBase = NULL;
}

/* --- Main loop ------------------------------------------------------------
 *
 * A plain Delay()-based poll rather than multiplexing a WaitSelect()-style
 * signal mask with a timer.device signal: this program only needs to
 * notice a commodity command or a due publish tick within about a second,
 * which a ~1s poll already gives it - not worth the extra complexity of a
 * second armed IORequest alongside the one already open for uptime/
 * scheduling. Same idiom as sub_main_lib.c's own MQTT_GetMessage() poll
 * loop. */
static void run_loop(APTR client, CxObj *broker, struct MsgPort *cxport,
                      const ha_device *dev, ULONG interval_secs)
{
    int running = 1;
    int enabled = 1;
    ULONG last_publish = uptime_secs();

    for (;;) {
        if (cxport) {
            CxMsg *cxm;
            while ((cxm = (CxMsg *) GetMsg(cxport)) != NULL) {
                ULONG type = CxMsgType(cxm);
                ULONG id = CxMsgID(cxm);
                ReplyMsg((struct Message *) cxm);
                if (type != CXM_COMMAND)
                    continue;
                switch (id) {
                case CXCMD_ENABLE:
                    if (!enabled) {
                        enabled = 1;
                        publish_availability(client, dev, "online");
                    }
                    if (broker)
                        ActivateCxObj(broker, 1);
                    break;
                case CXCMD_DISABLE:
                    if (enabled) {
                        enabled = 0;
                        publish_availability(client, dev, "offline");
                    }
                    if (broker)
                        ActivateCxObj(broker, 0);
                    break;
                case CXCMD_KILL:
                    running = 0;
                    break;
                default:
                    break;
                }
            }
        }
        if (!running)
            break;

        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
            SetSignal(0, SIGBREAKF_CTRL_C);
            break;
        }

        if (enabled) {
            ULONG now = uptime_secs();
            /* uptime_secs() only ever counts forward from system reset -
             * no clock-jump hazard here (see this file's own timer.device
             * comment), so plain subtraction is safe without the
             * wraparound guarding tool_now_ms() callers need. */
            if (now - last_publish >= interval_secs) {
                publish_state(client, dev);
                last_publish = now;
            }
        }

        Delay(MQTTSTATS_POLL_TICKS);
    }
}

int main(int argc, char **argv)
{
    STRPTR *tt;
    mqttstats_config cfg;
    char node_id_buf[64];
    ha_device dev;
    struct MsgPort *cxport = NULL;
    CxObj *broker = NULL;
    APTR client = NULL;
    struct MqttConnectOpts opts;
    ULONG keepalive;
    int exit_code = 0;

    /* argc==0 (Workbench launch) makes ArgArrayInit() pull ToolTypes from
     * the WBStartup message libnix's crt0 stashed instead - see this
     * file's own banner. Shell launch (argc>0) reads KEYWORD=VALUE
     * arguments off argv the same way. */
    tt = ArgArrayInit((LONG) argc, (CONST_STRPTR *) argv);
    read_config(tt, &cfg, node_id_buf, sizeof(node_id_buf));

    if (!cfg.host) {
        raw_str("mqttstats: HOST tooltype/argument is required\r\n");
        ArgArrayDone();
        return 20;
    }

    dev.node_id = (const char *) cfg.node_id;
    dev.device_name = (const char *) cfg.device_name;

    if (open_timer() < 0) {
        raw_str("mqttstats: can't open timer.device\r\n");
        ArgArrayDone();
        return 20;
    }

    CxBase = OpenLibrary((STRPTR) "commodities.library", 37);
    if (!CxBase) {
        raw_str("mqttstats: can't open commodities.library\r\n");
        close_timer();
        ArgArrayDone();
        return 20;
    }

    cxport = CreateMsgPort();
    if (cxport) {
        struct NewBroker nb;
        LONG cberr = 0;

        memset(&nb, 0, sizeof(nb));
        nb.nb_Version = NB_VERSION;
        nb.nb_Name = (STRPTR) "mqttstats";
        nb.nb_Title = (STRPTR) "midge mqttstats";
        /* Exchange's list truncates this on OS 3.1 well before the end -
         * "Home Assistant" needs to land safely inside that limit rather
         * than run off the end of the string, or it gets cut mid-word
         * ("...to Home Ass"). "Home Assistant" ends at character 33 here,
         * comfortably inside the ~37-character cutoff observed on 3.1. */
        nb.nb_Descr = (STRPTR) "Amiga telemetry to Home Assistant via MQTT";
        nb.nb_Unique = NBU_UNIQUE;
        nb.nb_Flags = 0; /* no window - nothing for Exchange to Show/Hide */
        nb.nb_Pri = cfg.cx_priority;
        nb.nb_Port = cxport;

        broker = CxBroker(&nb, &cberr);
        if (!broker && cberr == CBERR_DUP) {
            /* Another mqttstats is already running - exit quietly, same
             * as a second midge_pub instance would just publish twice for
             * no benefit. */
            DeleteMsgPort(cxport);
            CloseLibrary(CxBase);
            close_timer();
            ArgArrayDone();
            return 0;
        }
        if (broker) {
            ActivateCxObj(broker, 1);
        } else {
            DeleteMsgPort(cxport);
            cxport = NULL;
        }
    }

    MqttBase = OpenLibrary((STRPTR) "mqtt.library", 0);
    if (!MqttBase) {
        raw_str("mqttstats: can't open mqtt.library - copy it to LIBS: first\r\n");
        exit_code = 20;
        goto cleanup_cx;
    }

    /* Non-fatal: workbench_version() reports "unavailable" if this fails -
     * see that function's own comment. */
    WorkbenchBase = OpenLibrary((STRPTR) "workbench.library", 0);

    /* Keepalive comfortably above our own publish interval (see run_loop()'s
     * banner): our periodic PUBLISHes alone keep the broker's keepalive
     * tracking satisfied, since ANY MQTT packet counts, not specifically a
     * PINGREQ - mqtt.library's connection child is idle (blocked in
     * WaitPort()) between our commands, so it never sends one on its own. */
    keepalive = cfg.interval_secs * 2;
    if (keepalive < 60)
        keepalive = 60;
    if (keepalive > 65535)
        keepalive = 65535;

    memset(&opts, 0, sizeof(opts));
    opts.mco_ClientID = cfg.client_id;
    opts.mco_Username = cfg.username;
    opts.mco_Password = cfg.password;
    opts.mco_KeepAlive = (UWORD) keepalive;
    opts.mco_CleanSession = TRUE;
    opts.mco_AutoReconnect = TRUE; /* a background daemon should ride out drops */
    opts.mco_TLS = cfg.tls;
    opts.mco_TLSInsecure = cfg.tls_insecure;
    opts.mco_CAFile = cfg.ca_file;

    {
        ULONG backoff_secs = 1;

        for (;;) {
            LONG rc;

            client = MQTT_CreateClient(cfg.host, cfg.port, &opts);
            if (client) {
                rc = MQTT_Connect(client);
                if (rc == 0)
                    break; /* connected - fall through below */
                if (rc == MQTT_CONNECT_REFUSED) {
                    alert_connect_refused((const char *) cfg.host);
                    MQTT_DeleteClient(client);
                    client = NULL;
                    exit_code = 20;
                    goto cleanup_lib;
                }
                MQTT_DeleteClient(client);
                client = NULL;
            }
            raw_str("mqttstats: connect failed, retrying...\r\n");
            if (wait_ticks_or_kill(cxport, backoff_secs * 50)) {
                /* Killed via Exchange, or Ctrl-C from a Shell launch,
                 * before ever connecting - a clean exit, not an error. */
                goto cleanup_lib;
            }
            if (backoff_secs < 32)
                backoff_secs *= 2;
        }
    }

    publish_discovery(client, &dev);
    publish_availability(client, &dev, "online");
    publish_state(client, &dev);

    run_loop(client, broker, cxport, &dev, cfg.interval_secs);

    publish_availability(client, &dev, "offline");
    MQTT_Disconnect(client);

    MQTT_DeleteClient(client);
cleanup_lib:
    if (WorkbenchBase)
        CloseLibrary(WorkbenchBase);
    CloseLibrary(MqttBase);
cleanup_cx:
    if (broker)
        DeleteCxObjAll(broker);
    if (cxport) {
        struct Message *m;
        while ((m = GetMsg(cxport)) != NULL)
            ReplyMsg(m);
        DeleteMsgPort(cxport);
    }
    if (CxBase)
        CloseLibrary(CxBase);
    close_timer();
    ArgArrayDone();
    return exit_code;
}
