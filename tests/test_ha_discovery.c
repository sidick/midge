#include "test.h"

#include <string.h>

#include "ha_discovery.h"

static void test_topics(void)
{
    ha_device dev = { "a1200", "Amiga" };
    char buf[256];
    int n;

    n = ha_discovery_topic(&dev, "uptime", buf, sizeof(buf));
    TEST_CHECK(n > 0);
    TEST_CHECK(strcmp(buf, "homeassistant/sensor/midge_a1200/uptime/config") == 0);

    n = ha_state_topic(&dev, "chip_free", buf, sizeof(buf));
    TEST_CHECK(n > 0);
    TEST_CHECK(strcmp(buf, "midge/a1200/chip_free") == 0);

    n = ha_availability_topic(&dev, buf, sizeof(buf));
    TEST_CHECK(n > 0);
    TEST_CHECK(strcmp(buf, "midge/a1200/status") == 0);
}

static void test_discovery_payload_with_unit_and_class(void)
{
    ha_device dev = { "a1200", "Amiga" };
    char buf[512];
    int n = ha_discovery_payload(&dev, "uptime", "Amiga Uptime", "s",
                                  "duration", buf, sizeof(buf));

    TEST_CHECK(n > 0);
    TEST_CHECK(strstr(buf, "\"name\":\"Amiga Uptime\"") != NULL);
    TEST_CHECK(strstr(buf, "\"unique_id\":\"midge_a1200_uptime\"") != NULL);
    TEST_CHECK(strstr(buf, "\"state_topic\":\"midge/a1200/uptime\"") != NULL);
    TEST_CHECK(strstr(buf, "\"availability_topic\":\"midge/a1200/status\"") != NULL);
    TEST_CHECK(strstr(buf, "\"payload_available\":\"online\"") != NULL);
    TEST_CHECK(strstr(buf, "\"payload_not_available\":\"offline\"") != NULL);
    TEST_CHECK(strstr(buf, "\"unit_of_measurement\":\"s\"") != NULL);
    TEST_CHECK(strstr(buf, "\"device_class\":\"duration\"") != NULL);
    TEST_CHECK(strstr(buf, "\"identifiers\":[\"midge_a1200\"]") != NULL);
    TEST_CHECK(strstr(buf, "\"name\":\"Amiga\"") != NULL); /* the device block's own name */
}

static void test_discovery_payload_omits_unit_and_class(void)
{
    ha_device dev = { "a1200", "Amiga" };
    char buf[512];
    int n = ha_discovery_payload(&dev, "cpu_model", "Amiga CPU Model", NULL,
                                  NULL, buf, sizeof(buf));

    TEST_CHECK(n > 0);
    TEST_CHECK(strstr(buf, "unit_of_measurement") == NULL);
    TEST_CHECK(strstr(buf, "device_class") == NULL);
}

static void test_discovery_payload_escapes_device_name(void)
{
    ha_device dev = { "a1200", "Simon's \"Amiga\"" };
    char buf[512];
    int n = ha_discovery_payload(&dev, "uptime", "Uptime", NULL, NULL, buf,
                                  sizeof(buf));

    TEST_CHECK(n > 0);
    TEST_CHECK(strstr(buf, "\"name\":\"Simon's \\\"Amiga\\\"\"") != NULL);
}

static void test_bufsize_errors(void)
{
    ha_device dev = { "a1200", "Amiga" };
    char tiny[4];

    TEST_CHECK(ha_discovery_topic(&dev, "uptime", tiny, sizeof(tiny)) == -1);
    TEST_CHECK(ha_state_topic(&dev, "uptime", tiny, sizeof(tiny)) == -1);
    TEST_CHECK(ha_availability_topic(&dev, tiny, sizeof(tiny)) == -1);
    TEST_CHECK(ha_discovery_payload(&dev, "uptime", "Uptime", "s", "duration",
                                     tiny, sizeof(tiny)) == -1);
}

void run_ha_discovery_tests(void)
{
    test_topics();
    test_discovery_payload_with_unit_and_class();
    test_discovery_payload_omits_unit_and_class();
    test_discovery_payload_escapes_device_name();
    test_bufsize_errors();
}
