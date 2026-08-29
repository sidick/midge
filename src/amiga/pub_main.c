#include <stdio.h>

#include "args.h"
#include "tool_opts.h"
#include "transport_bsdsocket.h"
#include "version.h"

MIDGE_VERSTAG("mqtt_pub-static")

int main(void)
{
    tool_opts opts;
    mqtt_transport transport;
    bsdsocket_ctx ctx;
    int rc;

    if (amiga_parse_args(1, &opts) != 0)
        return 20; /* RETURN_ERROR */

    if (transport_bsdsocket_connect(&transport, &ctx, opts.host, opts.port) != 0) {
        fprintf(stderr, "mqtt_pub: cannot connect to %s\n", opts.host);
        amiga_args_cleanup();
        return 20;
    }

    rc = mqtt_pub_run(&opts, &transport);
    amiga_args_cleanup();
    return rc;
}
