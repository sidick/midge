#include <signal.h>
#include <stdio.h>

#include "args.h"
#include "tool_opts.h"
#include "transport_bsd.h"
#include "transport_openssl.h"

int main(int argc, char **argv)
{
    tool_opts opts;
    mqtt_transport transport;
    bsd_ctx ctx;
    openssl_ctx ossl_ctx;
    int connected;

    /* See pub_main.c's identical signal(SIGPIPE, SIG_IGN) comment. */
    signal(SIGPIPE, SIG_IGN);

    if (host_parse_args(argc, argv, 0, &opts) != 0)
        return 2;

    if (opts.tls) {
        connected = transport_openssl_connect(&transport, &ossl_ctx,
                                               opts.host, opts.port,
                                               opts.tls_insecure) == 0;
    } else {
        connected = transport_bsd_connect(&transport, &ctx, opts.host,
                                           opts.port) == 0;
    }
    if (!connected) {
        fprintf(stderr, "mqtt_sub: cannot connect to %s:%u\n", opts.host,
                (unsigned)opts.port);
        return 1;
    }

    return mqtt_sub_run(&opts, &transport);
}
