/* test_main.c — runner. Each module contributes a run_*_tests() entry point. */
#include "test.h"

test_ctx g_test = { 0, 0 };

void run_codec_tests(void);
void run_client_tests(void);
void run_ha_discovery_tests(void);

int main(void)
{
    run_codec_tests();
    run_client_tests();
    run_ha_discovery_tests();

    printf("\n%d passed, %d failed\n", g_test.passed, g_test.failed);

    return g_test.failed ? 1 : 0;
}
