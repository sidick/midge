/* libtls.c — on-target (m68k/AmigaOS) end-to-end TLS smoke test for
 * mqtt.library's mco_TLS/mco_TLSInsecure (issue #3 Phase 3): OpenLibrary,
 * CreateClient with mco_TLS set, Connect, Subscribe, GetMessage, Publish,
 * Disconnect, DeleteClient, CloseLibrary, all against a real TLS Mosquitto
 * reachable over Copperline's HostSocket board - mqtt.library's TLS
 * counterpart to libnet.c (see that file's banner for the shared shape;
 * this one differs only in needing AmiSSL and a real AmigaOS boot instead
 * of the bundled-AROS/HostSocket-only boot libnet.c uses - see
 * tests/library/README.md's "TLS smoke test" section for why).
 *
 * Same PASS/FAIL/RESULT/END serial contract as libnet.c/libsmoke.c/
 * codec_selftest.c, via exec/RawPutChar rather than stdio.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/mqtt.h>
#include <proto/mqtt.h>

#include <string.h>

/* See libnet.c's banner: a normal CLI program must define + fill this in
 * itself, unlike a library's own base. */
struct Library *MqttBase;

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

/* Must match tls-run.sh's own PORT/topic/payload constants exactly. */
#define TEST_HOST         "127.0.0.1"
#define TEST_PORT         18884
#define TOPIC_IN          "midge/lib/tls/in"
#define TOPIC_OUT         "midge/lib/tls/out"
#define RETAINED_PAYLOAD  "hello-from-host-retained-tls"
#define OUT_PAYLOAD       "hello-from-mqtt-library-tls"

/* Poll budget for the retained message - see libnet.c's own comment. A
 * real TLS handshake costs more than a plaintext connect, so this budget
 * is a little more generous than libnet.c's. */
#define POLL_TRIES  150
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
    opts.mco_ClientID = (STRPTR) "midge-libtls";
    opts.mco_KeepAlive = 30;
    opts.mco_CleanSession = TRUE;
    opts.mco_TLS = TRUE;
    /* Self-signed test broker (tls-run.sh) - not a production posture, see
     * mco_TLSInsecure's own doc comment in <libraries/mqtt.h>. */
    opts.mco_TLSInsecure = TRUE;

    client = MQTT_CreateClient((STRPTR) TEST_HOST, TEST_PORT, &opts);
    check(client != NULL, "createclient-nonnull");
    if (!client)
        goto close_lib;

    check(MQTT_Connect(client) == 0, "connect-tls-ok");
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

    check(MQTT_Publish(client, (STRPTR) TOPIC_OUT, (APTR) OUT_PAYLOAD,
                        (ULONG) strlen(OUT_PAYLOAD), 0, 1) == 0,
          "publish-qos1-ok");

    MQTT_Disconnect(client);
    MQTT_DeleteClient(client);

close_lib:
    CloseLibrary(MqttBase);

done:
    raw_str(g_fails == 0 ? "RESULT=OK\r\n" : "RESULT=FAIL\r\n");
    raw_str("END\r\n");
    return g_fails;
}
