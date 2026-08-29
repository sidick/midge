#include "args.h"

#include <dos/dos.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#define PUB_TEMPLATE                                                       \
    "HOST/A,PORT/N/K,TOPIC/A,MESSAGE/K,FILE/K,QOS/N/K,CLIENTID/K,USER/K,"  \
    "PASSWORD/K,KEEPALIVE/N/K,RETAIN/S,VERBOSE/S"
#define SUB_TEMPLATE                                                       \
    "HOST/A,PORT/N/K,TOPIC/A,QOS/N/K,CLIENTID/K,USER/K,PASSWORD/K,"        \
    "KEEPALIVE/N/K,COUNT/N/K,VERBOSE/S"

enum {
    PUB_HOST, PUB_PORT, PUB_TOPIC, PUB_MESSAGE, PUB_FILE, PUB_QOS,
    PUB_CLIENTID, PUB_USER, PUB_PASSWORD, PUB_KEEPALIVE, PUB_RETAIN,
    PUB_VERBOSE, PUB_NARGS
};
enum {
    SUB_HOST, SUB_PORT, SUB_TOPIC, SUB_QOS, SUB_CLIENTID, SUB_USER,
    SUB_PASSWORD, SUB_KEEPALIVE, SUB_COUNT, SUB_VERBOSE, SUB_NARGS
};

static struct RDArgs *g_rdargs;

int amiga_parse_args(int is_pub, tool_opts *opts)
{
    LONG args[PUB_NARGS]; /* PUB_NARGS >= SUB_NARGS; one array covers both */
    const char *template = is_pub ? PUB_TEMPLATE : SUB_TEMPLATE;
    int nargs = is_pub ? PUB_NARGS : SUB_NARGS;
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->port = 1883;
    opts->keepalive = 60;

    for (i = 0; i < nargs; i++)
        args[i] = 0;

    g_rdargs = ReadArgs((STRPTR)template, args, NULL);
    if (!g_rdargs) {
        PrintFault(IoErr(), (STRPTR) "midge");
        return -1;
    }

    if (is_pub) {
        opts->host = (const char *)args[PUB_HOST];
        if (args[PUB_PORT])
            opts->port = (uint16_t)*(LONG *)args[PUB_PORT];
        opts->topic = (const char *)args[PUB_TOPIC];
        opts->message = (const char *)args[PUB_MESSAGE];
        opts->file = (const char *)args[PUB_FILE];
        if (args[PUB_QOS])
            opts->qos = (uint8_t)*(LONG *)args[PUB_QOS];
        opts->client_id = (const char *)args[PUB_CLIENTID];
        opts->username = (const char *)args[PUB_USER];
        opts->password = (const char *)args[PUB_PASSWORD];
        if (args[PUB_KEEPALIVE])
            opts->keepalive = (uint16_t)*(LONG *)args[PUB_KEEPALIVE];
        opts->retain = args[PUB_RETAIN] ? 1 : 0;
        opts->verbose = args[PUB_VERBOSE] ? 1 : 0;

        if (!opts->message && !opts->file) {
            fprintf(stderr, "mqtt_pub: MESSAGE or FILE is required\n");
            amiga_args_cleanup();
            return -1;
        }
        if (opts->message && opts->file) {
            fprintf(stderr,
                    "mqtt_pub: MESSAGE and FILE are mutually exclusive\n");
            amiga_args_cleanup();
            return -1;
        }
    } else {
        opts->host = (const char *)args[SUB_HOST];
        if (args[SUB_PORT])
            opts->port = (uint16_t)*(LONG *)args[SUB_PORT];
        opts->topic = (const char *)args[SUB_TOPIC];
        if (args[SUB_QOS])
            opts->qos = (uint8_t)*(LONG *)args[SUB_QOS];
        opts->client_id = (const char *)args[SUB_CLIENTID];
        opts->username = (const char *)args[SUB_USER];
        opts->password = (const char *)args[SUB_PASSWORD];
        if (args[SUB_KEEPALIVE])
            opts->keepalive = (uint16_t)*(LONG *)args[SUB_KEEPALIVE];
        if (args[SUB_COUNT])
            opts->count = (int)*(LONG *)args[SUB_COUNT];
        opts->verbose = args[SUB_VERBOSE] ? 1 : 0;
    }

    if (opts->qos > 1) {
        fprintf(stderr, "midge: QoS 2 is not supported (see docs/PROTOCOL.md)\n");
        amiga_args_cleanup();
        return -1;
    }

    return 0;
}

void amiga_args_cleanup(void)
{
    if (g_rdargs) {
        FreeArgs(g_rdargs);
        g_rdargs = NULL;
    }
}
