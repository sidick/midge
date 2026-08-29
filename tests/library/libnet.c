/* libnet.c — on-target (m68k/AmigaOS) end-to-end network smoke test for
 * mqtt.library's real API (Phase 2 slice 2): OpenLibrary, CreateClient,
 * Connect, Subscribe, GetMessage, Publish, Disconnect, DeleteClient,
 * CloseLibrary, all against a real broker reachable over Copperline's
 * HostSocket board (see ../net/README.md - same board, same reasoning;
 * this is mqtt.library's counterpart to tests/net/'s mqtt_pub check).
 *
 * Unlike libsmoke.c, this is a NORMAL libnix CLI program (main(), the
 * usual crt0/startup - NOT the library skeleton itself) built against the
 * generated caller-side headers (proto/mqtt.h + inline/mqtt.h, from
 * `make library-headers`), exactly as any other mqtt.library client would
 * be. It still emits PASS/FAIL lines over the serial port via
 * exec/RawPutChar (same ROM debug path as libsmoke.c/codec_selftest.c)
 * rather than using stdio, purely to keep the pass/fail contract
 * diff-comparable with the other on-target tests and avoid dragging in a
 * stdio window.
 *
 * See net-run.sh for how the broker side (retained pre-publish to
 * TOPIC_IN, an observer subscribed to TOPIC_OUT) is set up before this
 * program runs.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/mqtt.h>
#include <proto/mqtt.h>

#include <string.h>

/* MqttBase: the inline/mqtt.h macros sfdc generated (build/include/inline/
 * mqtt.h) call through whatever C identifier MQTT_BASE_NAME names - the
 * default is the bare identifier `MqttBase` (see proto/mqtt.h's own
 * `extern struct Library *MqttBase;`). A normal CLI program has to define
 * that global itself and fill it in via OpenLibrary() - unlike a library's
 * own base (set up by the OS before __UserLibInit runs), there is no
 * auto-open for a library libnix doesn't know about at link time. */
struct Library *MqttBase;

/* exec RawPutChar: char in d0, SysBase (absolute 4) in a6, LVO -516. Same
 * hand-rolled helper as libsmoke.c/codec_selftest.c - see those files'
 * banners for why RawPutChar rather than stdio. */
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

/* Must match net-run.sh's own PORT/topic/payload constants exactly. */
#define TEST_HOST         "127.0.0.1"
#define TEST_PORT         18832
#define TOPIC_IN          "midge/lib/in"
#define TOPIC_OUT         "midge/lib/out"
#define RETAINED_PAYLOAD  "hello-from-host-retained"
#define OUT_PAYLOAD       "hello-from-mqtt-library"
#define OUT_PAYLOAD_QOS0  "midge-qos0-second-payload"

/* Poll budget for the retained message: ~20s at Delay(10) (0.2s) per try,
 * per the task's "up to ~20s" allowance - generous next to the sub-second
 * retained delivery a local Mosquitto actually needs. */
#define POLL_TRIES  100
#define POLL_TICKS  10

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
    opts.mco_ClientID = (STRPTR) "midge-libnet";
    opts.mco_KeepAlive = 30;
    opts.mco_CleanSession = TRUE;

    client = MQTT_CreateClient((STRPTR) TEST_HOST, TEST_PORT, &opts);
    check(client != NULL, "createclient-nonnull");
    if (!client)
        goto close_lib;

    check(MQTT_Connect(client) == 0, "connect-ok");
    /* Subscribe at QoS 1 (still receives the QoS 0 retained publish below
     * at delivery QoS 0 - MQTT delivery QoS is min(subscribe qos, publish
     * qos) - the point here is exercising the SUBACK-wait path added for
     * this milestone). */
    check(MQTT_Subscribe(client, (STRPTR) TOPIC_IN, 1) == 0, "subscribe-ok");

    for (i = 0; i < POLL_TRIES; i++) {
        msg = MQTT_GetMessage(client);
        if (msg)
            break;
        Delay(POLL_TICKS);
    }
    check(msg != NULL, "getmessage-received");

    if (msg) {
        check(strcmp((const char *)msg->mm_Topic, TOPIC_IN) == 0,
              "message-topic");
        check(strcmp((const char *)msg->mm_Payload, RETAINED_PAYLOAD) == 0,
              "message-payload");
        MQTT_FreeMessage(client, msg);
    } else {
        check(0, "message-topic");
        check(0, "message-payload");
    }

    /* QoS 1: MQTT_Publish() only returns 0 once the broker's PUBACK has
     * actually arrived (see mqtt_funcs.c/libraries/mqtt.h) - a real round
     * trip against a real broker, not just "encoded and sent". */
    check(MQTT_Publish(client, (STRPTR) TOPIC_OUT, (APTR) OUT_PAYLOAD,
                        (ULONG) strlen(OUT_PAYLOAD), 0, 1) == 0,
          "publish-qos1-ok");

    /* QoS 0: still fire-and-forget - returns once written to the
     * transport, no broker round trip. */
    check(MQTT_Publish(client, (STRPTR) TOPIC_OUT, (APTR) OUT_PAYLOAD_QOS0,
                        (ULONG) strlen(OUT_PAYLOAD_QOS0), 0, 0) == 0,
          "publish-qos0-ok");

    /* The QoS 0 publish only guarantees the child has encoded+sent by the
     * time it replies (see mqtt_funcs.c) - give the host observer a
     * little slack to actually see it on the wire before we tear the
     * connection down. */
    Delay(25);

    MQTT_Disconnect(client);
    MQTT_DeleteClient(client);

close_lib:
    CloseLibrary(MqttBase);

done:
    raw_str(g_fails == 0 ? "RESULT=OK\r\n" : "RESULT=FAIL\r\n");
    raw_str("END\r\n");
    return g_fails;
}
