/* pub_main_lib.c - the default, library-linked mqtt_pub: same ReadArgs
 * template/CLI contract as the static tool (src/amiga/pub_main.c /
 * src/tools/mqtt_pub.c), but drives mqtt.library's MQTT_* API
 * (<libraries/mqtt.h>/<proto/mqtt.h>) instead of linking src/core directly
 * over a bsdsocket transport. Because the library supports QoS 0 AND 1
 * (unlike the static tool, which rejects anything but QoS 0 - see
 * src/tools/mqtt_pub.c), QOS/N/K actually does something useful here.
 *
 * Deliberately NOT linked against src/core or src/tools - this file (plus
 * src/amiga/args.c, reused unchanged) is the entire program; see the
 * Makefile's `m68k` target for the exact link line. */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/mqtt.h>
#include <proto/mqtt.h>

#include <stdio.h>
#include <string.h>

#include "args.h"
#include "tool_opts.h"
#include "version.h"

MIDGE_VERSTAG("mqtt_pub")

/* Every mqtt.library caller must define this global itself - see
 * examples/pubexample.c's banner for why (no libnix auto-open for a
 * library the linker doesn't know about). */
struct Library *MqttBase;

/* Same size cap semantics as src/tools/mqtt_pub.c's MQTT_TOOL_BUF_SIZE -
 * kept off the ~4KB shell stack (CLAUDE.md). */
#define PUB_LIB_BUF_SIZE 8320

static long read_file(const char *path, unsigned char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return -1;
    n = fread(buf, 1, cap, f);
    if (!feof(f)) { /* more data than `cap` - refuse a silent truncation */
        fclose(f);
        return -1;
    }
    fclose(f);
    return (long)n;
}

int main(void)
{
    static unsigned char payload[PUB_LIB_BUF_SIZE];
    tool_opts opts;
    struct MqttConnectOpts co;
    APTR client;
    LONG rc;
    long payload_len = 0;
    int exit_code = 0;

    if (amiga_parse_args(1, &opts) != 0)
        return 20; /* RETURN_ERROR */

    if (opts.qos > 1) {
        /* amiga_parse_args() does no range check of its own on QOS/N/K
         * (src/amiga/args.c just copies the LONG), so this is the check
         * that actually rejects QoS 2+ - QoS 2 is out of scope for the
         * whole project (docs/PROTOCOL.md). */
        fprintf(stderr, "mqtt_pub: QoS %u is not supported\n",
                (unsigned)opts.qos);
        amiga_args_cleanup();
        return 20;
    }

    if (opts.file) {
        payload_len = read_file(opts.file, payload, sizeof(payload));
        if (payload_len < 0) {
            fprintf(stderr, "mqtt_pub: cannot read %s (or it exceeds %d bytes)\n",
                    opts.file, PUB_LIB_BUF_SIZE);
            amiga_args_cleanup();
            return 20;
        }
    } else if (opts.message) {
        payload_len = (long)strlen(opts.message);
        if ((size_t)payload_len > sizeof(payload)) {
            fprintf(stderr, "mqtt_pub: message exceeds %d bytes\n",
                    PUB_LIB_BUF_SIZE);
            amiga_args_cleanup();
            return 20;
        }
        memcpy(payload, opts.message, (size_t)payload_len);
    }

    MqttBase = OpenLibrary((STRPTR) "mqtt.library", 0);
    if (!MqttBase) {
        fprintf(stderr, "mqtt_pub: can't open mqtt.library - "
                         "copy it to LIBS: first\n");
        amiga_args_cleanup();
        return 20;
    }

    memset(&co, 0, sizeof(co));
    if (opts.client_id)
        co.mco_ClientID = (STRPTR)opts.client_id;
    /* NULL is fine: the library treats an absent client id the same way
     * the static tool's core does - an empty CONNECT client id, asking the
     * broker to assign one (see mqtt_funcs.c). */
    if (opts.username) {
        co.mco_Username = (STRPTR)opts.username;
        if (opts.password)
            co.mco_Password = (STRPTR)opts.password;
    }
    co.mco_KeepAlive = opts.keepalive;
    co.mco_CleanSession = TRUE;
    co.mco_AutoReconnect = FALSE;

    if (opts.verbose)
        printf("mqtt_pub: connecting to %s:%u\n", opts.host,
               (unsigned)opts.port);

    client = MQTT_CreateClient((STRPTR)opts.host, opts.port, &co);
    if (!client) {
        fprintf(stderr, "mqtt_pub: MQTT_CreateClient failed "
                         "(out of memory?)\n");
        exit_code = 20;
        goto close_lib;
    }

    rc = MQTT_Connect(client);
    if (rc != 0) {
        fprintf(stderr, "mqtt_pub: connect failed (code %ld)\n", (long)rc);
        exit_code = 20;
        goto delete_client;
    }

    if (opts.verbose)
        printf("mqtt_pub: publishing to %s (%ld bytes)\n", opts.topic,
               payload_len);

    rc = MQTT_Publish(client, (STRPTR)opts.topic, payload,
                       (ULONG)payload_len, opts.retain ? TRUE : FALSE,
                       opts.qos);
    if (rc != 0) {
        fprintf(stderr, "mqtt_pub: publish failed (code %ld)\n", (long)rc);
        exit_code = 20;
    }

    MQTT_Disconnect(client);

delete_client:
    MQTT_DeleteClient(client);

close_lib:
    CloseLibrary(MqttBase);
    amiga_args_cleanup();
    return exit_code;
}
