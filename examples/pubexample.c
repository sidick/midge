/* pubexample.c - minimal mqtt.library caller: connect, publish one message
 * at QoS 1, disconnect.
 *
 * This is a normal AmigaDOS CLI program (not a test harness) meant to be
 * read as documentation for third-party mqtt.library callers - it uses
 * ordinary stdio (printf) and dos.library ReadArgs(), not the RawPutChar/
 * serial-port tricks the project's own on-target tests use to talk to
 * Copperline. Build it exactly as an external developer would: it only
 * ever includes <libraries/mqtt.h> and <proto/mqtt.h> (both under
 * build/include, see the Makefile's `library-headers` target) plus plain
 * NDK/libc headers - nothing from this repository's src/ tree, other than
 * src/version.h for the $VER string below (a build-time convenience only;
 * a real third party would just hardcode their own version string here
 * instead).
 *
 * CLI only: like every other tool in this project, running this from
 * Workbench (no argv) is out of scope - use the Shell.
 *
 * Usage:
 *   pubexample HOST PORT TOPIC MESSAGE
 *   pubexample 192.168.1.10 1883 home/amiga/hello "hello from midge"
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <libraries/mqtt.h>
#include <proto/mqtt.h>

#include <stdio.h>
#include <string.h>

/* build-time-only include, not an API header - see the file banner above. */
#include "version.h"

MIDGE_VERSTAG("pubexample")

/* Every mqtt.library caller must define this global itself: the sfdc-
 * generated inline/mqtt.h macros (build/include/inline/mqtt.h) call
 * through it, and unlike a library's own base, there is no libnix auto-open
 * for a library the linker doesn't know about. */
struct Library *MqttBase;

#define TEMPLATE "HOST/A,PORT/N/A,TOPIC/A,MESSAGE/A"
enum { ARG_HOST, ARG_PORT, ARG_TOPIC, ARG_MESSAGE, ARG_COUNT };

int main(void)
{
    LONG args[ARG_COUNT] = { 0, 0, 0, 0 };
    struct RDArgs *rdargs;
    STRPTR host, topic, message;
    UWORD port;
    struct MqttConnectOpts opts;
    APTR client;
    LONG rc;
    int exit_code = 0;

    rdargs = ReadArgs((STRPTR) TEMPLATE, args, NULL);
    if (!rdargs) {
        PrintFault(IoErr(), (STRPTR) "pubexample");
        return 20;
    }

    host = (STRPTR) args[ARG_HOST];
    port = (UWORD) *(LONG *) args[ARG_PORT];
    topic = (STRPTR) args[ARG_TOPIC];
    message = (STRPTR) args[ARG_MESSAGE];

    /* mqtt.library is an ordinary shared library: OpenLibrary() it like any
     * other, and always check for NULL - it may not be installed, or (with
     * a version argument other than 0 here) may be too old. */
    MqttBase = OpenLibrary((STRPTR) "mqtt.library", 0);
    if (!MqttBase) {
        fprintf(stderr, "pubexample: can't open mqtt.library - "
                         "copy it to LIBS: first\n");
        FreeArgs(rdargs);
        return 20;
    }

    printf("pubexample: connecting to %s:%ld ...\n", (const char *) host,
           (long) port);

    /* MQTT_CreateClient() deep-copies every string in `opts`, so it is safe
     * to let it go out of scope (or be reused) right after the call - see
     * <libraries/mqtt.h>. A zeroed struct is a fine default: no username/
     * password, clean session, no auto-reconnect (see subexample.c for
     * that option). */
    memset(&opts, 0, sizeof(opts));
    opts.mco_ClientID = (STRPTR) "midge-pubexample";
    opts.mco_KeepAlive = 60;
    opts.mco_CleanSession = TRUE;

    client = MQTT_CreateClient(host, port, &opts);
    if (!client) {
        fprintf(stderr, "pubexample: MQTT_CreateClient failed "
                         "(out of memory?)\n");
        exit_code = 20;
        goto close_lib;
    }

    rc = MQTT_Connect(client);
    if (rc != 0) {
        fprintf(stderr, "pubexample: MQTT_Connect failed: %ld\n", (long) rc);
        exit_code = 20;
        goto delete_client;
    }
    printf("pubexample: connected\n");

    /* QoS 1: MQTT_Publish() only returns once the broker's PUBACK has
     * actually arrived (retrying a few times first if it doesn't) - see
     * <libraries/mqtt.h> for the exact retry budget. retain = FALSE. */
    printf("pubexample: publishing to \"%s\": \"%s\"\n",
           (const char *) topic, (const char *) message);
    rc = MQTT_Publish(client, topic, message,
                       (ULONG) strlen((const char *) message), FALSE, 1);
    if (rc != 0) {
        fprintf(stderr, "pubexample: MQTT_Publish failed: %ld\n", (long) rc);
        exit_code = 20;
    } else {
        printf("pubexample: published (broker acknowledged)\n");
    }

    MQTT_Disconnect(client);

delete_client:
    /* MQTT_DeleteClient() also disconnects if still connected, so calling
     * MQTT_Disconnect() first above is optional - it is only here so the
     * "connected" / "disconnected" progress messages bracket the publish
     * cleanly. */
    MQTT_DeleteClient(client);

close_lib:
    CloseLibrary(MqttBase);
    FreeArgs(rdargs);
    return exit_code;
}
