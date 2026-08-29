/* libreconn.c — on-target (m68k/AmigaOS) end-to-end test for
 * mco_AutoReconnect (Phase 2 "reconnect/backoff + auto-resubscribe"
 * milestone): proves the client's subprocess notices an unexpected
 * connection drop, backs off, reconnects, and automatically re-issues its
 * subscriptions - all without the caller doing anything beyond the initial
 * MQTT_Connect()/MQTT_Subscribe().
 *
 * Modeled directly on libnet.c (same RawPutChar PASS/FAIL/RESULT/END
 * harness, same "normal libnix CLI program built against the generated
 * caller-side headers" shape) - see that file's banner for the rationale.
 * The broker-outage choreography itself lives in reconn-run.sh: it kills
 * and restarts the scratch Mosquitto (same port) between two retained
 * publishes to TOPIC_IN, so this guest sees the retained "phase1" payload
 * immediately on subscribe, then only sees the retained "phase2" payload if
 * it actually reconnected and resubscribed after the outage.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/mqtt.h>
#include <proto/mqtt.h>

#include <string.h>

/* See libnet.c's banner for why this global exists (inline/mqtt.h's
 * macros call through the bare `MqttBase` identifier; a normal CLI program
 * has no auto-open for a library libnix doesn't know about at link time). */
struct Library *MqttBase;

/* exec RawPutChar: char in d0, SysBase (absolute 4) in a6, LVO -516. */
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

static int g_fails;

static void check(int cond, const char *name)
{
    if (cond) {
        raw_str("PASS ");
    } else {
        raw_str("FAIL ");
        g_fails++;
    }
    raw_str(name);
    raw_str("\r\n");
}

/* Must match reconn-run.sh's own PORT/topic/payload constants exactly. */
#define TEST_HOST         "127.0.0.1"
#define TEST_PORT         18833
#define TOPIC_IN          "midge/lib/reconn"
#define PHASE1_PAYLOAD    "reconn-phase1-before-outage"
#define PHASE2_PAYLOAD    "reconn-phase2-after-outage"

/* Poll budget for the phase1 retained message: same shape as libnet.c's
 * own POLL_TRIES/POLL_TICKS (~20s at Delay(10)/0.2s per try) - the initial
 * SUBACK already proves the first broker is up, so phase1 delivery is
 * near-instant. */
#define POLL1_TRIES 100
#define POLL1_TICKS 10

/* Poll budget for the phase2 retained message, published by reconn-run.sh
 * only *after* killing and restarting the broker: generous enough to cover
 * the backoff schedule's early retries (1s, 2s, 4s, 8s, 16s, 32s, 32s, ...)
 * plus reconn-run.sh's own broker-restart choreography - see that script's
 * banner. ~120s emulated at Delay(10)/0.2s per try. */
#define POLL2_TRIES 600
#define POLL2_TICKS 10

int main(void)
{
    struct MqttConnectOpts opts;
    APTR client = NULL;
    struct MqttMessage *msg = NULL;
    int i;

    raw_str("BEGIN\r\n");

    MqttBase = OpenLibrary((STRPTR) "mqtt.library", 0);
    check(MqttBase != NULL, "openlibrary-nonnull");
    if (!MqttBase)
        goto done;

    memset(&opts, 0, sizeof(opts));
    opts.mco_ClientID = (STRPTR) "midge-libreconn";
    opts.mco_KeepAlive = 10;
    opts.mco_CleanSession = TRUE;
    opts.mco_AutoReconnect = TRUE;

    client = MQTT_CreateClient((STRPTR) TEST_HOST, TEST_PORT, &opts);
    check(client != NULL, "createclient-nonnull");
    if (!client)
        goto close_lib;

    check(MQTT_Connect(client) == 0, "connect-ok");
    check(MQTT_Subscribe(client, (STRPTR) TOPIC_IN, 0) == 0, "subscribe-ok");

    for (i = 0; i < POLL1_TRIES; i++) {
        msg = MQTT_GetMessage(client);
        if (msg)
            break;
        Delay(POLL1_TICKS);
    }
    check(msg != NULL, "phase1-received");
    if (msg) {
        check(strcmp((const char *)msg->mm_Topic, TOPIC_IN) == 0,
              "phase1-topic");
        check(strcmp((const char *)msg->mm_Payload, PHASE1_PAYLOAD) == 0,
              "phase1-payload");
        MQTT_FreeMessage(client, msg);
        msg = NULL;
    } else {
        check(0, "phase1-topic");
        check(0, "phase1-payload");
    }

    /* Distinctive marker line: reconn-run.sh polls the serial capture for
     * this before killing the broker, so the outage only starts once the
     * guest has actually subscribed and drained the phase1 retained
     * message (otherwise a too-early kill could race the phase1 check
     * above against the broker restart below). */
    raw_str("MARK ready-for-outage\r\n");

    /* reconn-run.sh now kills the broker, waits for it to die, starts a
     * fresh one on the same port, and retained-publishes PHASE2_PAYLOAD.
     * Any MQTT_Publish()/MQTT_Subscribe() attempted here while the guest's
     * child subprocess is still reconnecting would fail fast with
     * MQTTERR_NOTCONNECTED (see libraries/mqtt.h) - this test only polls
     * MQTT_GetMessage(), which never fails, so no such call is needed:
     * receiving PHASE2_PAYLOAD at all proves the child noticed the drop,
     * backed off, reconnected, and auto-resubscribed TOPIC_IN entirely on
     * its own. */
    for (i = 0; i < POLL2_TRIES; i++) {
        msg = MQTT_GetMessage(client);
        if (msg)
            break;
        Delay(POLL2_TICKS);
    }
    check(msg != NULL, "reconnect-received");
    if (msg) {
        check(strcmp((const char *)msg->mm_Topic, TOPIC_IN) == 0,
              "reconnect-topic");
        check(strcmp((const char *)msg->mm_Payload, PHASE2_PAYLOAD) == 0,
              "reconnect-payload");
        MQTT_FreeMessage(client, msg);
    } else {
        check(0, "reconnect-topic");
        check(0, "reconnect-payload");
    }

    MQTT_Disconnect(client);
    MQTT_DeleteClient(client);

close_lib:
    CloseLibrary(MqttBase);

done:
    raw_str(g_fails == 0 ? "RESULT=OK\r\n" : "RESULT=FAIL\r\n");
    raw_str("END\r\n");
    return g_fails;
}
