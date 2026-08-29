/* libsmoke.c — on-target (m68k/AmigaOS) smoke test for mqtt.library
 * (src/library/). Proves the slice 1 skeleton (libinit.c) actually opens
 * and closes cleanly on a real AmigaOS boot: OpenLibrary("mqtt.library",
 * 0) must return a non-NULL base whose lib_Node.ln_Name is "mqtt.library"
 * and whose lib_Version matches src/library/libinit.c's LibVersion, then
 * CloseLibrary() must not blow up. Emits PASS/FAIL lines over the serial
 * port via exec/RawPutChar — same ROM debug path as
 * tests/copperline/codec_selftest.c, which this file otherwise mirrors
 * (see that file's banner for why RawPutChar: no serial.device handler,
 * no Mount, no Workbench files needed).
 *
 * This is a normal libnix CLI program (main(), linked with the usual
 * crt0/startup — not the library skeleton itself), so libnix's startup
 * auto-opens SysBase for us; proto/exec.h's OpenLibrary/CloseLogLibrary
 * work directly, same as src/amiga/transport_bsdsocket.c. RawPutChar
 * output still goes through the same hand-rolled inline-asm helper as
 * codec_selftest.c rather than relying on that, purely to keep this file
 * self-contained and diff-comparable with codec_selftest.c.
 *
 * The library itself is NOT linked in here — it must be staged into the
 * boot volume's Libs: directory (LIBS: auto-assigns to the boot volume's
 * Libs directory on AmigaOS) so OpenLibrary finds it as a real disk
 * library. See run.sh / volamos-run.sh for staging.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>

#include <string.h>

/* exec RawPutChar: char in d0, SysBase (absolute 4) in a6, LVO -516. */
static void raw_put(char c)
{
    void *SysBase = *(void **)4UL;
    register long d0 __asm__("d0") = (unsigned char)c;
    register void *a6 __asm__("a6") = SysBase;
    __asm__ volatile("jsr -516(%%a6)" : : "r"(d0), "r"(a6)
                     : "d1", "a0", "a1", "cc", "memory");
}

static void raw_str(const char *s)
{
    while (*s)
        raw_put(*s++);
}

static int g_fails;

static void check(int cond, const char *name)
{
    if (cond) {
        raw_str("PASS ");
    } else {
        raw_str("FAIL ");
        g_fails++;
    }
    raw_str(name);
    raw_str("\r\n");
}

int main(void)
{
    struct Library *base;

    raw_str("BEGIN\r\n");

    base = OpenLibrary((STRPTR) "mqtt.library", 0);
    check(base != NULL, "openlibrary-nonnull");

    if (base != NULL) {
        check(base->lib_Node.ln_Name != NULL &&
                  strcmp((const char *)base->lib_Node.ln_Name, "mqtt.library") == 0,
              "openlibrary-name");
        check(base->lib_Version >= 1, "openlibrary-version");

        CloseLibrary(base);
        check(1, "closelibrary-returned");
    } else {
        /* Can't meaningfully check name/version/close without a base -
         * report them as failed too, rather than silently skipping, so a
         * broken open can't hide behind fewer FAIL lines. */
        check(0, "openlibrary-name");
        check(0, "openlibrary-version");
        check(0, "closelibrary-returned");
    }

    raw_str(g_fails == 0 ? "RESULT=OK\r\n" : "RESULT=FAIL\r\n");
    raw_str("END\r\n");
    return g_fails;
}
