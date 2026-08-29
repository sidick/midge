#include "tool_clock.h"

#include <time.h>

uint32_t tool_now_ms(void)
{
    return (uint32_t)time(NULL) * 1000u;
}
