#include "tool_clock.h"

#include <dos/dos.h>
#include <proto/dos.h>

/* Standard Amiga system tick rate (dos/dos.h defines TICKS_PER_SECOND on
 * some NDK versions but not all bebbo amiga-gcc header sets - hardcode the
 * value AmigaOS has used since 1.x rather than depend on it). */
#define MIDGE_TICKS_PER_SECOND 50

uint32_t tool_now_ms(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (uint32_t)((uint32_t)ds.ds_Days * 1440u + (uint32_t)ds.ds_Minute) *
               60000u +
           ((uint32_t)ds.ds_Tick * 1000u) / MIDGE_TICKS_PER_SECOND;
}
