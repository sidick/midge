#ifndef MIDGE_HA_DISCOVERY_H
#define MIDGE_HA_DISCOVERY_H

#include <stddef.h>

/* Home Assistant MQTT Discovery topic/payload builders (issue #6's
 * telemetry-publishing commodity, mqttstats). Portable C99, no OS
 * dependencies - the same split as src/core: this is the part worth
 * unit-testing byte-exact on the host, kept apart from the Amiga-only
 * glue (ToolTypes, CxBroker, AvailMem/AttnFlags telemetry gathering,
 * mqtt.library calls) that actually calls it, in src/amiga/mqttstats_main.c.
 *
 * See https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery for
 * the wire format this targets. */

typedef struct {
    const char *node_id;     /* short, topic-safe identifier for this Amiga
                                 (e.g. derived from CLIENTID) - used in every
                                 topic name and in each sensor's unique_id */
    const char *device_name; /* friendly name shown in HA's device list */
} ha_device;

/* Every function below writes a NUL-terminated string into `buf` (`cap`
 * bytes) and returns the length written (excluding the NUL) on success, or
 * -1 if `cap` was too small (nothing is written in that case - never a
 * silent truncation). */

/* "homeassistant/sensor/midge_<node_id>/<key>/config" - where retained
 * discovery config payloads are published (once, at connect). */
int ha_discovery_topic(const ha_device *dev, const char *key,
                        char *buf, size_t cap);

/* The discovery config JSON payload for one sensor. `unit` and
 * `device_class` are Home Assistant fields; pass NULL for either to omit
 * it from the payload (some sensors, e.g. a text value, have neither). */
int ha_discovery_payload(const ha_device *dev, const char *key,
                          const char *friendly_name, const char *unit,
                          const char *device_class, char *buf, size_t cap);

/* "midge/<node_id>/<key>" - where this sensor's current value is
 * published on every publish tick. */
int ha_state_topic(const ha_device *dev, const char *key,
                    char *buf, size_t cap);

/* "midge/<node_id>/status" - "online"/"offline", referenced by every
 * sensor's availability_topic (see ha_discovery_payload()). */
int ha_availability_topic(const ha_device *dev, char *buf, size_t cap);

#endif
