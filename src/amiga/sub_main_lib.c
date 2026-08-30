/* sub_main_lib.c - the default, library-linked mqtt_sub: same ReadArgs
 * template/CLI contract as the static tool (src/amiga/sub_main.c /
 * src/tools/mqtt_sub.c), but drives mqtt.library's MQTT_* API
 * (<libraries/mqtt.h>/<proto/mqtt.h>) via a poll loop instead of linking
 * src/core directly over a bsdsocket transport.
 *
 * Output format is kept byte-for-byte identical to mqtt_sub_run()'s
 * on_publish() (src/tools/mqtt_sub.c): VERBOSE prints "topic " then the
 * payload via fwrite() (binary-safe - payloads are not necessarily text)
 * then a newline, flushed after every message so tests/scripts that grep
 * output from a file (or a live pipe) keep working unchanged.
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

MIDGE_VERSTAG("mqtt_sub")

/* Every mqtt.library caller must define this global itself - see
 * examples/subexample.c's banner for why. */
struct Library *MqttBase;

/* Delay() ticks are 1/50s; a 0.2s poll interval between empty polls, same
 * as examples/subexample.c - short enough to notice Ctrl-C and new
 * messages promptly, long enough not to busy-loop the CPU. */
#define POLL_TICKS 10

int main(void)
{
    tool_opts opts;
    struct MqttConnectOpts co;
    APTR client;
    LONG rc;
    int seen = 0;
    int exit_code = 0;

    if (amiga_parse_args(0, &opts) != 0)
        return 20; /* RETURN_ERROR */

    MqttBase = OpenLibrary((STRPTR) "mqtt.library", 0);
    if (!MqttBase) {
        fprintf(stderr, "mqtt_sub: can't open mqtt.library - "
                         "copy it to LIBS: first\n");
        amiga_args_cleanup();
        return 20;
    }

    memset(&co, 0, sizeof(co));
    if (opts.client_id)
        co.mco_ClientID = (STRPTR)opts.client_id;
    if (opts.username) {
        co.mco_Username = (STRPTR)opts.username;
        if (opts.password)
            co.mco_Password = (STRPTR)opts.password;
    }
    co.mco_KeepAlive = opts.keepalive;
    co.mco_CleanSession = TRUE;
    co.mco_AutoReconnect = FALSE;
    co.mco_TLS = opts.tls ? TRUE : FALSE;
    co.mco_TLSInsecure = opts.tls_insecure ? TRUE : FALSE;

    if (opts.verbose)
        printf("mqtt_sub: connecting to %s:%u\n", opts.host,
               (unsigned)opts.port);

    client = MQTT_CreateClient((STRPTR)opts.host, opts.port, &co);
    if (!client) {
        fprintf(stderr, "mqtt_sub: MQTT_CreateClient failed "
                         "(out of memory?)\n");
        exit_code = 20;
        goto close_lib;
    }

    rc = MQTT_Connect(client);
    if (rc != 0) {
        fprintf(stderr, "mqtt_sub: connect failed (code %ld)\n", (long)rc);
        exit_code = 20;
        goto delete_client;
    }

    rc = MQTT_Subscribe(client, (STRPTR)opts.topic, opts.qos);
    if (rc != 0) {
        fprintf(stderr, "mqtt_sub: subscribe failed (code %ld)\n", (long)rc);
        exit_code = 20;
        goto disconnect;
    }
    if (opts.verbose)
        printf("mqtt_sub: subscribed to %s (QoS %u)\n", opts.topic,
               (unsigned)opts.qos);

    /* SetSignal(0,0) just reads the current signal mask without changing
     * it, so a Ctrl-C already latched before this loop started (e.g. while
     * ReadArgs()/MQTT_Connect() were running) is still seen - same pattern
     * as examples/subexample.c. */
    while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
        struct MqttMessage *msg = MQTT_GetMessage(client);

        if (msg) {
            if (opts.verbose)
                printf("%s ", (const char *)msg->mm_Topic);
            fwrite(msg->mm_Payload, 1, msg->mm_PayloadLen, stdout);
            printf("\n");
            fflush(stdout);
            MQTT_FreeMessage(client, msg);
            seen++;
            if (opts.count > 0 && seen >= opts.count)
                break;
            continue; /* drain the queue before sleeping again */
        }
        Delay(POLL_TICKS);
    }
    /* Clear the signal so nothing downstream sees a stale Ctrl-C pending. */
    SetSignal(0, SIGBREAKF_CTRL_C);

disconnect:
    MQTT_Disconnect(client);

delete_client:
    MQTT_DeleteClient(client);

close_lib:
    CloseLibrary(MqttBase);
    amiga_args_cleanup();
    return exit_code;
}
