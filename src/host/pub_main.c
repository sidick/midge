#include <signal.h>
#include <stdio.h>

#include "args.h"
#include "tool_opts.h"
#include "transport_bsd.h"

int main(int argc, char **argv)
{
    tool_opts opts;
    mqtt_transport transport;
    bsd_ctx ctx;

    /* Sending on a connection the broker has already closed raises SIGPIPE,
     * which kills the process by default; MSG_NOSIGNAL isn't portable
     * (missing on macOS), so ignore it here instead - send()'s normal -1/
     * EPIPE return is what transport_bsd.c already handles. */
    signal(SIGPIPE, SIG_IGN);

    if (host_parse_args(argc, argv, 1, &opts) != 0)
        return 2;

    if (transport_bsd_connect(&transport, &ctx, opts.host, opts.port) != 0) {
        fprintf(stderr, "mqtt_pub: cannot connect to %s:%u\n", opts.host,
                (unsigned)opts.port);
        return 1;
    }

    return mqtt_pub_run(&opts, &transport);
}
