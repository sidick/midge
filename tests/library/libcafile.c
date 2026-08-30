/* libcafile.c — on-target end-to-end mco_CAFile test (issue #13): proves
 * that a broker behind a private CA (a) fails certificate verification
 * without the extra trust anchor, and (b) succeeds with mco_CAFile
 * pointing at the CA's certificate. Same shape/serial contract as
 * libtls.c - see that file's banner and tests/library/README.md's "TLS
 * smoke test" section (this test shares its amibake-image/local-only
 * requirement, plus a seeded Kickstart RTC - see cafile-run.sh's banner
 * for why that matters).
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/mqtt.h>
#include <proto/mqtt.h>

#include <string.h>

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

/* Must match cafile-run.sh's own PORT/CA-file path exactly. */
#define TEST_HOST  "127.0.0.1"
#define TEST_PORT  18886
#define CA_FILE    "AmiSSL:Certs/midge-test-ca.pem"

static APTR connect_with(STRPTR ca_file)
{
    struct MqttConnectOpts opts;
    APTR client;

    memset(&opts, 0, sizeof(opts));
    opts.mco_ClientID = (STRPTR) "midge-libcafile";
    opts.mco_KeepAlive = 30;
    opts.mco_CleanSession = TRUE;
    opts.mco_TLS = TRUE;
    opts.mco_CAFile = ca_file;

    client = MQTT_CreateClient((STRPTR) TEST_HOST, TEST_PORT, &opts);
    if (!client)
        return NULL;
    if (MQTT_Connect(client) != 0) {
        MQTT_DeleteClient(client);
        return NULL;
    }
    return client;
}

int main(void)
{
    APTR client;

    raw_str("BEGIN\r\n");

    MqttBase = OpenLibrary((STRPTR) "mqtt.library", 0);
    check(MqttBase != NULL, "openlibrary-nonnull");
    if (!MqttBase)
        goto done;

    /* Without the CA file: the broker's cert is signed by a CA AmiSSL's
     * bundled trust store has never heard of - verification MUST fail. */
    client = connect_with(NULL);
    check(client == NULL, "no-cafile-connect-fails");
    if (client) {
        MQTT_Disconnect(client);
        MQTT_DeleteClient(client);
    }

    /* With it: same broker, same cert - now trusted. */
    client = connect_with((STRPTR) CA_FILE);
    check(client != NULL, "with-cafile-connect-ok");
    if (client) {
        MQTT_Disconnect(client);
        MQTT_DeleteClient(client);
    }

    CloseLibrary(MqttBase);

done:
    raw_str(g_fails == 0 ? "RESULT=OK\r\n" : "RESULT=FAIL\r\n");
    raw_str("END\r\n");
    return g_fails;
}
