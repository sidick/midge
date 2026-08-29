#ifndef MIDGE_HOST_ARGS_H
#define MIDGE_HOST_ARGS_H

#include "tool_opts.h"

/* Parses argv (mosquitto_pub/sub-style short flags) into `opts`, which is
 * zeroed and given defaults (port 1883, keepalive 60s) before parsing.
 * `is_pub` selects the accepted flag set (mqtt_pub also takes -m/-f/-r;
 * mqtt_sub takes -C). Prints its own usage/error message to stderr on
 * failure. Returns 0 on success, -1 on bad input. */
int host_parse_args(int argc, char **argv, int is_pub, tool_opts *opts);

#endif
