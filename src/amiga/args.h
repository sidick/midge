#ifndef MIDGE_AMIGA_ARGS_H
#define MIDGE_AMIGA_ARGS_H

#include "tool_opts.h"

/* Parses the Shell command line via dos.library ReadArgs() into `opts`.
 * `is_pub` selects the template (mqtt_pub also takes MESSAGE/FILE/RETAIN;
 * mqtt_sub takes COUNT). Prints its own error via PrintFault() on failure.
 * Returns 0 on success, -1 on failure.
 *
 * On success, `opts`'s string fields alias memory owned by ReadArgs() -
 * they stay valid until amiga_args_cleanup() is called, which the caller
 * must do exactly once, after it is done using `opts` (typically right
 * before the program exits). */
int amiga_parse_args(int is_pub, tool_opts *opts);
void amiga_args_cleanup(void);

#endif
