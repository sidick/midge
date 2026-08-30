/* glibc hides getopt/optind/optarg (POSIX.1-2001) under a strict -std=c99
 * build unless a feature-test macro says otherwise; must be defined before
 * the first system header - see transport_bsd.c's longer note (same root
 * cause, same CI-only failure on Linux, not local macOS development). */
#define _POSIX_C_SOURCE 200112L

#include "args.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int host_parse_args(int argc, char **argv, int is_pub, tool_opts *opts)
{
    int ch;
    const char *optstring = is_pub ? "h:p:t:m:f:q:i:u:P:k:rvsSc:"
                                    : "h:p:t:q:i:u:P:k:C:vsSc:";

    memset(opts, 0, sizeof(*opts));
    /* opts->port stays 0 (memset) until resolved after the option loop,
     * once we know whether -s/-S was given. */
    opts->keepalive = 60;

    optind = 1;
    while ((ch = getopt(argc, argv, optstring)) != -1) {
        switch (ch) {
        case 'h': opts->host = optarg; break;
        case 'p': opts->port = (uint16_t)atoi(optarg); break;
        case 't': opts->topic = optarg; break;
        case 'm': opts->message = optarg; break;
        case 'f': opts->file = optarg; break;
        case 'q': opts->qos = (uint8_t)atoi(optarg); break;
        case 'i': opts->client_id = optarg; break;
        case 'u': opts->username = optarg; break;
        case 'P': opts->password = optarg; break;
        case 'k': opts->keepalive = (uint16_t)atoi(optarg); break;
        case 'r': opts->retain = 1; break;
        case 'C': opts->count = atoi(optarg); break;
        case 'v': opts->verbose = 1; break;
        case 's': opts->tls = 1; break;
        case 'S': opts->tls = 1; opts->tls_insecure = 1; break;
        case 'c': opts->ca_file = optarg; break;
        default:
            fprintf(stderr,
                    is_pub ? "usage: %s -h host [-p port] -t topic "
                             "(-m message | -f file) [-q qos] [-i id] "
                             "[-u user] [-P pass] [-k keepalive] [-r] [-s] "
                             "[-S] [-c cafile] [-v]\n"
                           : "usage: %s -h host [-p port] -t topic "
                             "[-q qos] [-i id] [-u user] [-P pass] "
                             "[-k keepalive] [-C count] [-s] [-S] "
                             "[-c cafile] [-v]\n",
                    argv[0]);
            return -1;
        }
    }

    if (opts->port == 0)
        opts->port = opts->tls ? 8883 : 1883;

    if (!opts->host || !opts->topic) {
        fprintf(stderr, "%s: -h and -t are required\n", argv[0]);
        return -1;
    }
    if (is_pub && !opts->message && !opts->file) {
        fprintf(stderr, "%s: -m or -f is required\n", argv[0]);
        return -1;
    }
    if (is_pub && opts->message && opts->file) {
        fprintf(stderr, "%s: -m and -f are mutually exclusive\n", argv[0]);
        return -1;
    }
    if (opts->qos > 1) {
        fprintf(stderr, "%s: QoS 2 is not supported (see docs/PROTOCOL.md)\n",
                argv[0]);
        return -1;
    }
    return 0;
}
