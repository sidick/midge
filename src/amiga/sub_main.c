#include <stdio.h>

#include "args.h"
#include "tool_opts.h"
#include "transport_bsdsocket.h"
#include "version.h"

MIDGE_VERSTAG("mqtt_sub-static")

int main(void)
{
    tool_opts opts;
    mqtt_transport transport;
    bsdsocket_ctx ctx;
    int rc;

    if (amiga_parse_args(0, &opts) != 0)
        return 20; /* RETURN_ERROR */

    /* This statically-linked build has no AmiSSL support - unlike the
     * default, mqtt.library-linked mqtt_sub (src/amiga/sub_main_lib.c) -
     * so TLS must be rejected outright rather than silently falling back
     * to a plaintext connection the caller didn't ask for. */
    if (opts.tls) {
        fprintf(stderr,
                "mqtt_sub: TLS not supported - this build has no AmiSSL "
                "(use the default mqtt_sub instead)\n");
        amiga_args_cleanup();
        return 20;
    }

    if (transport_bsdsocket_connect(&transport, &ctx, opts.host, opts.port) != 0) {
        fprintf(stderr, "mqtt_sub: cannot connect to %s\n", opts.host);
        amiga_args_cleanup();
        return 20;
    }

    rc = mqtt_sub_run(&opts, &transport);
    amiga_args_cleanup();
    return rc;
}
