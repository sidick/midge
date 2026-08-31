#include "ha_discovery.h"

#include <stdio.h>
#include <string.h>

/* JSON-escapes `src` into `dst` (`cap` bytes, NUL-terminated) - only `"`
 * and `\` need it for the fields this module ever puts through here
 * (device_name is the one genuinely free-text, caller-supplied field;
 * everything else is an internal constant or an already topic-safe
 * node_id). Truncates silently rather than overflowing if `src` is
 * pathologically long - a cut-off device name is a cosmetic problem, not
 * a wire-format one, unlike every other buffer in this file where running
 * out of room means the caller's `cap` was too small for a fixed template
 * and returning -1 is correct instead. */
static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t i = 0;

    if (cap == 0)
        return;
    cap--; /* room for the NUL */
    while (*src && i + 1 < cap) {
        if (*src == '"' || *src == '\\') {
            if (i + 2 >= cap)
                break;
            dst[i++] = '\\';
        }
        dst[i++] = *src++;
    }
    dst[i] = '\0';
}

int ha_discovery_topic(const ha_device *dev, const char *key,
                        char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "homeassistant/sensor/midge_%s/%s/config",
                      dev->node_id, key);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}

int ha_discovery_payload(const ha_device *dev, const char *key,
                          const char *friendly_name, const char *unit,
                          const char *device_class, char *buf, size_t cap)
{
    char name_esc[128];
    int n;

    json_escape(dev->device_name, name_esc, sizeof(name_esc));

    n = snprintf(buf, cap,
        "{\"name\":\"%s\",\"unique_id\":\"midge_%s_%s\","
        "\"state_topic\":\"midge/%s/%s\","
        "\"availability_topic\":\"midge/%s/status\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\"",
        friendly_name, dev->node_id, key,
        dev->node_id, key,
        dev->node_id);
    if (n < 0 || (size_t)n >= cap)
        return -1;

    if (unit) {
        int m = snprintf(buf + n, cap - (size_t)n,
                          ",\"unit_of_measurement\":\"%s\"", unit);
        if (m < 0 || (size_t)(n + m) >= cap)
            return -1;
        n += m;
    }
    if (device_class) {
        int m = snprintf(buf + n, cap - (size_t)n,
                          ",\"device_class\":\"%s\"", device_class);
        if (m < 0 || (size_t)(n + m) >= cap)
            return -1;
        n += m;
    }

    {
        int m = snprintf(buf + n, cap - (size_t)n,
            ",\"device\":{\"identifiers\":[\"midge_%s\"],\"name\":\"%s\","
            "\"manufacturer\":\"Commodore/Amiga\",\"model\":\"mqttstats\"}}",
            dev->node_id, name_esc);
        if (m < 0 || (size_t)(n + m) >= cap)
            return -1;
        n += m;
    }

    return n;
}

int ha_state_topic(const ha_device *dev, const char *key,
                    char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "midge/%s/%s", dev->node_id, key);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}

int ha_availability_topic(const ha_device *dev, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "midge/%s/status", dev->node_id);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}
