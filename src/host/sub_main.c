#include <stdio.h>

#include "args.h"
#include "tool_opts.h"
#include "transport_bsd.h"

int main(int argc, char **argv)
{
    tool_opts opts;
    mqtt_transport transport;
    bsd_ctx ctx;

    if (host_parse_args(argc, argv, 0, &opts) != 0)
        return 2;

    if (transport_bsd_connect(&transport, &ctx, opts.host, opts.port) != 0) {
        fprintf(stderr, "mqtt_sub: cannot connect to %s:%u\n", opts.host,
                (unsigned)opts.port);
        return 1;
    }

    return mqtt_sub_run(&opts, &transport);
}
