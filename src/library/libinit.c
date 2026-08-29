/* src/library/libinit.c - mqtt.library shared-library skeleton and function
 * table (Phase 2 slice 2).
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
 *   - The private function table (ADD2LIST/ADDTABL_END below): _InitTab in
 *     libinit.o points at a linker "set" named `___FuncTable__` built via
 *     `.stabs` pseudo-ops (see sys-include/stabs.h - this toolchain
 *     targets classic hunk/stabs, not the ELF variant of that header).
 *     mqtt_lib.sfd's 8 public functions (src/library/mqtt_funcs.c) are
 *     wired in below, in sfd order, followed by the required -1
 *     terminator - see the big comment above the ADD2LIST() calls.
 *
 * The MQTT logic itself lives in src/library/mqtt_funcs.c (the 8 sfd
 * entry points) and their generated `Gate_`-prefixed register trampolines
 * (build/library-gen/gatestubs.c, see the Makefile) - this file only
 * supplies the skeleton those hang off of.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* stabs.h's ADDTABL_END() expands to file-scope `asm(...)`. `asm` is only
 * a guaranteed keyword under GNU dialects; strict -std=c99 (this project's
 * convention, see the Makefile) drops it, so spell it out via the always-
 * available `__asm__` before pulling the macro in. */
#define asm __asm__
#include <stabs.h>

#include "version.h"

/* The library's own AmigaOS version (LibVersion.LibRevision, what
 * OpenLibrary()'s version argument checks against) is independent of
 * MIDGE_VERSION (the project release version stamped onto the CLI tools'
 * $VER, see version.h) - deliberately not tied to it, so the library's ABI
 * version only bumps when its interface actually changes. LibIdString and
 * the $VER string below must both spell out LibVersion.LibRevision (here,
 * "1.0"), per AmigaOS convention - keep all three in sync by hand if
 * LibVersion/LibRevision ever change. */
const BYTE LibName[]     = "mqtt.library";
const BYTE LibIdString[] = "mqtt.library 1.0 (" MIDGE_VERSION_DATE ")";
const UWORD LibVersion   = 1;
const UWORD LibRevision  = 0;

/* Amiga $VER string - findable by the Shell `Version` command and by
 * `strings` on the binary. Uses the literal "1.0" (matching
 * LibVersion.LibRevision above), NOT MIDGE_VERSTAG()'s MIDGE_VERSION (that
 * macro is for the CLI tools' own project-release $VER, e.g.
 * src/amiga/pub_main.c - the library's version numbers deliberately don't
 * track it, see the comment above). */
static const char verstag[] __attribute__((used)) =
    "\0$VER: mqtt.library 1.0 (" MIDGE_VERSION_DATE ")";

/* <proto/exec.h> only *declares* `extern struct ExecBase *SysBase;` - as a
 * library (not a program run through libnix's normal crt0/auto-open
 * startup, see amiga-integration.md) we have to define and set it
 * ourselves. libnix.a's own object code needs it too: linking pulled in
 * memcpy() from libnix.a (via src/core), and it references SysBase
 * directly (`nm libnix.a` shows `U _SysBase`) to call CopyMemQuick(). Per
 * CLAUDE.md, a library base pointer shadowing an NDK extern must not be
 * `static` - it has to be a real global so it satisfies that reference. */
struct ExecBase *SysBase;

/* <proto/dos.h> likewise only *declares* `extern struct DosLibrary
 * *DOSBase;` - opened here (never `static`, same reasoning as SysBase
 * above: it must be a real global to satisfy that extern for every object
 * linked into this library, including src/amiga/clock.c's tool_now_ms()
 * (DateStamp() needs it) and mqtt_funcs.c's CreateNewProcTags(). Slice 2
 * is the first to need it - slice 1's skeleton had nothing that called
 * into dos.library. */
struct DosLibrary *DOSBase;

int __UserLibInit(struct Library *base)
{
    (void)base;

    /* Absolute address 4 always holds SysBase on AmigaOS, in any context
     * (task, library init, interrupt) - the one fact you can rely on
     * before any library base exists. */
    SysBase = *(struct ExecBase **)4L;

    /* V37 (2.04): the oldest version with CreateNewProcTags() and every
     * dos.library call mqtt_funcs.c/clock.c need. Never open below 37 -
     * see CLAUDE.md/the libnix skill's __oslibversion note. Failing this
     * open aborts LibInit (0 == success is the only case that returns the
     * library node to the opener), so no caller ever gets a base with a
     * NULL DOSBase. */
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR) "dos.library", 37);
    if (!DOSBase)
        return 1;

    return 0; /* 0 == success; nonzero aborts LibInit and the library
                 node is not returned to the opener. */
}

void __UserLibCleanup(void)
{
    if (DOSBase) {
        CloseLibrary((struct Library *)DOSBase);
        DOSBase = NULL;
    }
}

/* Wires the 8 mqtt.library entry points (src/library/mqtt_lib.sfd) into
 * the private function table __UserLibInit's caller (libinit.o's
 * _LibInit) builds the library node from - see the file banner above for
 * the overall stabs-linker-set mechanism.
 *
 * Each ADD2LIST() call must appear here, in EXACT mqtt_lib.sfd order (the
 * table position is the function's LVO - -30 for the first entry below,
 * -36 for the next, and so on by 6, per ==bias 30), because the linker
 * collects a `.stabs` set's members in the order the assembler encounters
 * them - listing them together, in sequence, in this one file is what
 * keeps that order correct.
 *
 * `Gate_MQTT_*` (not the plain `MQTT_*` names) are what's referenced:
 * sfdc --mode=gatestubs --libarg=first --gateprefix=Gate_ (see the
 * Makefile) generates a register-parameter trampoline under that prefixed
 * name for each function - Gate_MQTT_CreateClient(host a0, port d0,
 * opts a1, base a6) - which is the actual jump-table entry AmigaOS calls;
 * it in turn calls the plain C function of the same un-prefixed name
 * (mqtt_funcs.c's MQTT_CreateClient(base, host, port, opts), normal C
 * calling convention) that does the real work. Without --gateprefix, the
 * generated trampoline and the plain function would collide on the same
 * symbol name and fail to link - verified by hand against sfdc 1.11f
 * before settling on this approach (see the task notes / git history for
 * the failing bare-name attempt). */
ADD2LIST(Gate_MQTT_CreateClient, __FuncTable__, 22);
ADD2LIST(Gate_MQTT_DeleteClient, __FuncTable__, 22);
ADD2LIST(Gate_MQTT_Connect, __FuncTable__, 22);
ADD2LIST(Gate_MQTT_Publish, __FuncTable__, 22);
ADD2LIST(Gate_MQTT_Subscribe, __FuncTable__, 22);
ADD2LIST(Gate_MQTT_GetMessage, __FuncTable__, 22);
ADD2LIST(Gate_MQTT_FreeMessage, __FuncTable__, 22);
ADD2LIST(Gate_MQTT_Disconnect, __FuncTable__, 22);

/* Terminates the function table. Required after the last ADD2LIST() call
 * above (or, in slice 1's case, with zero of them) - see the file banner. */
ADDTABL_END();
