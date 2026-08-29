#include <signal.h>
#include <stdio.h>

#include "args.h"
#include "tool_opts.h"
#include "transport_bsd.h"
#ifdef MIDGE_HOST_TLS
#include "transport_openssl.h"
#endif

int main(int argc, char **argv)
{
    tool_opts opts;
    mqtt_transport transport;
    bsd_ctx ctx;
#ifdef MIDGE_HOST_TLS
    openssl_ctx ossl_ctx;
#endif
    int connected;

    /* Sending on a connection the broker has already closed raises SIGPIPE,
     * which kills the process by default; MSG_NOSIGNAL isn't portable
     * (missing on macOS), so ignore it here instead - send()'s normal -1/
     * EPIPE return is what transport_bsd.c already handles. */
    signal(SIGPIPE, SIG_IGN);

    if (host_parse_args(argc, argv, 1, &opts) != 0)
        return 2;

#ifdef MIDGE_HOST_TLS
    if (opts.tls) {
        connected = transport_openssl_connect(&transport, &ossl_ctx,
                                               opts.host, opts.port,
                                               opts.tls_insecure) == 0;
    } else {
        connected = transport_bsd_connect(&transport, &ctx, opts.host,
                                           opts.port) == 0;
    }
#else
    if (opts.tls) {
        fprintf(stderr,
                "mqtt_pub: TLS not supported - this build has no OpenSSL\n");
        return 1;
    }
    connected = transport_bsd_connect(&transport, &ctx, opts.host,
                                       opts.port) == 0;
#endif
    if (!connected) {
        fprintf(stderr, "mqtt_pub: cannot connect to %s:%u\n", opts.host,
                (unsigned)opts.port);
        return 1;
    }

    return mqtt_pub_run(&opts, &transport);
}
