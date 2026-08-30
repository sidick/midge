#ifndef MIDGE_TOOL_OPTS_H
#define MIDGE_TOOL_OPTS_H

/* Shared mqtt_pub/mqtt_sub logic, driven by either front-end: the host's
 * getopt parser (src/host/args.c) or the Amiga ReadArgs parser
 * (src/amiga/args.c). Both populate the same tool_opts and hand it, plus an
 * already-connected transport, to the run function here - this is the only
 * code the two CLI tools actually share beyond src/core. */

#include <stdint.h>

#include "mqtt_transport.h"

typedef struct {
    const char *host;
    uint16_t port;      /* default 1883 plaintext, 8883 with TLS; parsers
                            leave 0 until resolved after the option loop */
    const char *topic;
    const char *message; /* mqtt_pub: literal payload; NULL if using `file` */
    const char *file;    /* mqtt_pub: read payload from this file instead */
    uint8_t qos;         /* 0 or 1; mqtt_pub only supports 0 in this release
                             (see mqtt_pub_run) */
    const char *client_id; /* NULL -> empty client id, broker-assigned */
    const char *username;  /* NULL if none */
    const char *password;  /* ignored without username */
    uint16_t keepalive;    /* seconds, default 60 */
    int retain;             /* mqtt_pub only */
    int verbose;
    int count;              /* mqtt_sub only; <= 0 means unlimited */
    int tls;                /* opt-in, never default-on (see issue #3) */
    int tls_insecure;       /* skip certificate verification; ignored
                                without tls */
    const char *ca_file;    /* extra trust anchor for a private CA (issue
                                #13); NULL uses only the system/AmiSSL
                                default trust store. Ignored without tls,
                                and ignored if tls_insecure is also set
                                (nothing to verify against then). */
} tool_opts;

/* Connects (assumes `transport` is already connected to the broker),
 * performs the tool's one job, and disconnects. Returns a process exit
 * code (0 on success). Prints its own progress/error messages to
 * stdout/stderr - this is CLI glue, not part of the portable core. */
int mqtt_pub_run(const tool_opts *opts, mqtt_transport *transport);
int mqtt_sub_run(const tool_opts *opts, mqtt_transport *transport);

#endif
