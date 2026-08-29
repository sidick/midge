/* src/library/libinit.c - mqtt.library shared-library skeleton (slice 1).
 *
 * Supplies the globals and hooks libnix's libinit.o (the single-data-
 * segment shared library startup - see the "libnix" skill,
 * references/shared-libs-devices.md) expects to link:
 *
 *   - LibName/LibIdString/LibVersion/LibRevision: read by _LibInit when it
 *     fills in the library node (see the romtag/_LibInit disassembly of
 *     libinit.o - `nm libinit.o` confirms the exact expected symbols).
 *   - __UserLibInit/__UserLibCleanup: called from _LibInit and
 *     _LibExpunge respectively. NOTE the exact spelling -
 *     `__UserLibCleanup`, lowercase "up" - `nm libinit.o` shows the
 *     linked symbol as `___UserLibCleanup`; the libnix guide text renders
 *     it "__UserLibCleanUp" in prose, which does not match the object
 *     file. Trust the tool, not the prose.
 *   - The private function table (ADDTABL_END below): _InitTab in
 *     libinit.o points at a linker "set" named `___FuncTable__` built via
 *     `.stabs` pseudo-ops (see sys-include/stabs.h - this toolchain
 *     targets classic hunk/stabs, not the ELF variant of that header).
 *     Slice 1 has no library functions yet (mqtt_lib.sfd is empty past
 *     the standard Open/Close/Expunge/ExtFunc four), but the list must
 *     still exist and be terminated with -1, or the linker has nothing to
 *     resolve _InitTab's reference to.
 *
 * Zero MQTT logic here by design - this is purely the skeleton that lets
 * OpenLibrary("mqtt.library", 0)/CloseLibrary() work cleanly. src/core is
 * linked into the library binary already (see the Makefile) so the layout
 * is ready for slice 2 to add real entry points to the .sfd and call into
 * it, but nothing calls it yet.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <proto/exec.h>

/* stabs.h's ADDTABL_END() expands to file-scope `asm(...)`. `asm` is only
 * a guaranteed keyword under GNU dialects; strict -std=c99 (this project's
 * convention, see the Makefile) drops it, so spell it out via the always-
 * available `__asm__` before pulling the macro in. */
#define asm __asm__
#include <stabs.h>

#include "version.h"

const BYTE LibName[]     = "mqtt.library";
const BYTE LibIdString[] = "mqtt.library " MIDGE_VERSION " (" MIDGE_VERSION_DATE ")";
const UWORD LibVersion   = 1;
const UWORD LibRevision  = 0;

/* Amiga $VER string - findable by the Shell `Version` command and by
 * `strings` on the binary, same convention as the CLI tools
 * (src/amiga/pub_main.c, src/amiga/sub_main.c). */
MIDGE_VERSTAG("mqtt.library")

/* <proto/exec.h> only *declares* `extern struct ExecBase *SysBase;` - as a
 * library (not a program run through libnix's normal crt0/auto-open
 * startup, see amiga-integration.md) we have to define and set it
 * ourselves. libnix.a's own object code needs it too: linking pulled in
 * memcpy() from libnix.a (via src/core), and it references SysBase
 * directly (`nm libnix.a` shows `U _SysBase`) to call CopyMemQuick(). Per
 * CLAUDE.md, a library base pointer shadowing an NDK extern must not be
 * `static` - it has to be a real global so it satisfies that reference. */
struct ExecBase *SysBase;

int __UserLibInit(struct Library *base)
{
    (void)base;

    /* Absolute address 4 always holds SysBase on AmigaOS, in any context
     * (task, library init, interrupt) - the one fact you can rely on
     * before any library base exists. */
    SysBase = *(struct ExecBase **)4L;

    return 0; /* 0 == success; nonzero aborts LibInit and the library
                 node is not returned to the opener. */
}

void __UserLibCleanup(void)
{
    /* Nothing to release yet - no MQTT state exists in slice 1. */
}

/* Terminates the (empty) private function table. Required even with zero
 * library functions - see the file banner above. */
ADDTABL_END();
