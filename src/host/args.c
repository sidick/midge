#include "args.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int host_parse_args(int argc, char **argv, int is_pub, tool_opts *opts)
{
    int ch;
    const char *optstring = is_pub ? "h:p:t:m:f:q:i:u:P:k:rv"
                                    : "h:p:t:q:i:u:P:k:C:v";

    memset(opts, 0, sizeof(*opts));
    opts->port = 1883;
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
        default:
            fprintf(stderr,
                    is_pub ? "usage: %s -h host [-p port] -t topic "
                             "(-m message | -f file) [-q qos] [-i id] "
                             "[-u user] [-P pass] [-k keepalive] [-r] [-v]\n"
                           : "usage: %s -h host [-p port] -t topic "
                             "[-q qos] [-i id] [-u user] [-P pass] "
                             "[-k keepalive] [-C count] [-v]\n",
                    argv[0]);
            return -1;
        }
    }

    if (!opts->host || !opts->topic) {
        fprintf(stderr, "%s: -h and -t are required\n", argv[0]);
        return -1;
    }
    if (is_pub && !opts->message && !opts->file) {
        fprintf(stderr, "%s: -m or -f is required\n", argv[0]);
        return -1;
    }
    if (opts->qos > 1) {
        fprintf(stderr, "%s: QoS 2 is not supported (see docs/PROTOCOL.md)\n",
                argv[0]);
        return -1;
    }
    return 0;
}
