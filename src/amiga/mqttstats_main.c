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

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/commodities.h>
#include <proto/mqtt.h>
#include <proto/timer.h>

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
struct Device *TimerBase;

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

/* timer.device's TR_GETSYSTIME reports seconds+micros since the last
 * system reset - genuinely monotonic, unlike dos.library's DateStamp()
 * (see tool_clock.h's own comment on issue #8). That distinction doesn't
 * matter for mqtt.library or the CLI tools (multi-connection subprocess
 * statics are off the table there), but this program is a standalone
 * singleton (NBU_UNIQUE enforces at most one instance), so opening the
 * device once at startup and keeping it for the process's lifetime is
 * completely safe - there is no second instance to trample it. Used for
 * both the reported "uptime" metric and this program's own interval
 * scheduling (avoiding the exact clock-jump hazard issue #8 documents). */
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

/* Seconds since the last system reset. */
static ULONG uptime_secs(void)
{
    g_timerreq->tr_node.io_Command = TR_GETSYSTIME;
    DoIO((struct IORequest *)g_timerreq);
    return (ULONG) g_timerreq->tr_time.tv_secs;
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
        nb.nb_Descr = (STRPTR) "Publishes Amiga telemetry to Home Assistant via MQTT";
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

    client = MQTT_CreateClient(cfg.host, cfg.port, &opts);
    if (!client) {
        raw_str("mqttstats: MQTT_CreateClient failed\r\n");
        exit_code = 20;
        goto cleanup_lib;
    }
    if (MQTT_Connect(client) != 0) {
        raw_str("mqttstats: connect failed\r\n");
        exit_code = 20;
        goto cleanup_client;
    }

    publish_discovery(client, &dev);
    publish_availability(client, &dev, "online");
    publish_state(client, &dev);

    run_loop(client, broker, cxport, &dev, cfg.interval_secs);

    publish_availability(client, &dev, "offline");
    MQTT_Disconnect(client);

cleanup_client:
    MQTT_DeleteClient(client);
cleanup_lib:
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
