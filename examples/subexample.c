/* subexample.c - minimal mqtt.library caller: connect with auto-reconnect
 * enabled, subscribe to one topic filter, and print messages as they
 * arrive until Ctrl-C.
 *
 * Like pubexample.c, this is a normal AmigaDOS CLI program meant to be read
 * as documentation, not a test harness: ordinary stdio, ReadArgs(), and
 * only <libraries/mqtt.h>/<proto/mqtt.h> plus plain NDK/libc headers (see
 * pubexample.c's banner for the full rationale, including why src/version.h
 * is the one exception). CLI only - no Workbench startup handling.
 *
 * Usage:
 *   subexample HOST PORT TOPIC
 *   subexample 192.168.1.10 1883 home/#
 *
 * Runs until interrupted with Ctrl-C (the Shell's usual break signal).
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/mqtt.h>
#include <proto/mqtt.h>

#include <stdio.h>
#include <string.h>

/* build-time-only include, not an API header - see pubexample.c's banner. */
#include "version.h"

MIDGE_VERSTAG("subexample")

/* Every mqtt.library caller must define this global itself - see
 * pubexample.c's banner for why. */
struct Library *MqttBase;

#define TEMPLATE "HOST/A,PORT/N/A,TOPIC/A"
enum { ARG_HOST, ARG_PORT, ARG_TOPIC, ARG_COUNT };

/* Delay() ticks are 1/50s, so this is a 0.2s poll interval - short enough
 * to notice Ctrl-C and new messages promptly, long enough not to busy-loop
 * the CPU. No buffers of any size live on the stack here (the shell's
 * default stack is a scarce ~4KB, per CLAUDE.md) - every message this
 * program looks at is library-allocated and reached only by pointer. */
#define POLL_TICKS 10

int main(void)
{
    LONG args[ARG_COUNT] = { 0, 0, 0 };
    struct RDArgs *rdargs;
    STRPTR host, topic;
    UWORD port;
    struct MqttConnectOpts opts;
    APTR client;
    LONG rc;
    int exit_code = 0;

    rdargs = ReadArgs((STRPTR) TEMPLATE, args, NULL);
    if (!rdargs) {
        PrintFault(IoErr(), (STRPTR) "subexample");
        return 20;
    }

    host = (STRPTR) args[ARG_HOST];
    port = (UWORD) *(LONG *) args[ARG_PORT];
    topic = (STRPTR) args[ARG_TOPIC];

    MqttBase = OpenLibrary((STRPTR) "mqtt.library", 0);
    if (!MqttBase) {
        fprintf(stderr, "subexample: can't open mqtt.library - "
                         "copy it to LIBS: first\n");
        FreeArgs(rdargs);
        return 20;
    }

    printf("subexample: connecting to %s:%ld ...\n", (const char *) host,
           (long) port);

    memset(&opts, 0, sizeof(opts));
    opts.mco_ClientID = (STRPTR) "midge-subexample";
    opts.mco_KeepAlive = 60;
    opts.mco_CleanSession = TRUE;
    /* Once MQTT_Connect() has succeeded, the client's own subprocess
     * quietly reconnects (exponential backoff, capped at 32s) and
     * re-subscribes every filter on any unexpected drop - no extra code
     * needed here. While reconnecting, MQTT_GetMessage() below just stops
     * producing new messages for a while; it never fails. See
     * <libraries/mqtt.h> for the full contract. */
    opts.mco_AutoReconnect = TRUE;

    client = MQTT_CreateClient(host, port, &opts);
    if (!client) {
        fprintf(stderr, "subexample: MQTT_CreateClient failed "
                         "(out of memory?)\n");
        exit_code = 20;
        goto close_lib;
    }

    rc = MQTT_Connect(client);
    if (rc != 0) {
        fprintf(stderr, "subexample: MQTT_Connect failed: %ld\n", (long) rc);
        exit_code = 20;
        goto delete_client;
    }
    printf("subexample: connected\n");

    rc = MQTT_Subscribe(client, topic, 1);
    if (rc != 0) {
        fprintf(stderr, "subexample: MQTT_Subscribe failed: %ld\n",
                (long) rc);
        exit_code = 20;
        goto disconnect;
    }
    printf("subexample: subscribed to \"%s\" - waiting for messages "
           "(Ctrl-C to stop)\n", (const char *) topic);

    /* SetSignal(0,0) just reads the current signal mask without changing
     * it, so any Ctrl-C already latched before this loop started (e.g. hit
     * while ReadArgs()/MQTT_Connect() were running) is still seen. */
    while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
        struct MqttMessage *msg = MQTT_GetMessage(client);

        if (msg) {
            printf("[%s] %s\n", (const char *) msg->mm_Topic,
                   (const char *) msg->mm_Payload);
            MQTT_FreeMessage(client, msg);
            continue; /* drain the queue before sleeping again */
        }
        Delay(POLL_TICKS);
    }
    /* Clear the signal so a subsequent Wait() elsewhere in a bigger
     * program (there is none here, but this is the polite habit) doesn't
     * see it as still pending. */
    SetSignal(0, SIGBREAKF_CTRL_C);
    printf("\nsubexample: Ctrl-C - shutting down\n");

disconnect:
    MQTT_Disconnect(client);

delete_client:
    MQTT_DeleteClient(client);

close_lib:
    CloseLibrary(MqttBase);
    FreeArgs(rdargs);
    return exit_code;
}
