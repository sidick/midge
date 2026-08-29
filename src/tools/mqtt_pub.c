#include "tool_opts.h"

#include <stdio.h>
#include <string.h>

#include "mqtt_client.h"
#include "tool_clock.h"

/* Default max packet size (proposal: 8KB covers smart-home payloads) plus
 * headroom for the fixed header - see mqtt_packet.h's note on `buf` sizing. */
#define MQTT_TOOL_BUF_SIZE 8320

static long read_file(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return -1;
    n = fread(buf, 1, cap, f);
    if (!feof(f)) { /* more data than `cap` - refuse a silent truncation */
        fclose(f);
        return -1;
    }
    fclose(f);
    return (long)n;
}

int mqtt_pub_run(const tool_opts *opts, mqtt_transport *transport)
{
    static uint8_t txbuf[MQTT_TOOL_BUF_SIZE];
    static uint8_t rxbuf[MQTT_TOOL_BUF_SIZE];
    static uint8_t payload[MQTT_TOOL_BUF_SIZE];
    mqtt_connect_opts co;
    mqtt_client c;
    mqtt_str topic;
    long payload_len = 0;
    uint32_t deadline;

    if (opts->qos != 0) {
        fprintf(stderr,
                "mqtt_pub: QoS %u publish is not supported in this release "
                "(outbound QoS 1 lands with mqtt.library); use -q 0\n",
                (unsigned)opts->qos);
        return 1;
    }

    memset(&co, 0, sizeof(co));
    if (opts->client_id) {
        co.client_id.ptr = opts->client_id;
        co.client_id.len = (uint16_t)strlen(opts->client_id);
    }
    co.clean_session = 1;
    co.keepalive = opts->keepalive;
    if (opts->username) {
        co.username.ptr = opts->username;
        co.username.len = (uint16_t)strlen(opts->username);
        if (opts->password) {
            co.password.ptr = opts->password;
            co.password.len = (uint16_t)strlen(opts->password);
        }
    }

    if (opts->file) {
        payload_len = read_file(opts->file, payload, sizeof(payload));
        if (payload_len < 0) {
            fprintf(stderr, "mqtt_pub: cannot read %s (or it exceeds %d bytes)\n",
                    opts->file, MQTT_TOOL_BUF_SIZE);
            return 1;
        }
    } else if (opts->message) {
        payload_len = (long)strlen(opts->message);
        if ((size_t)payload_len > sizeof(payload)) {
            fprintf(stderr, "mqtt_pub: message exceeds %d bytes\n",
                    MQTT_TOOL_BUF_SIZE);
            return 1;
        }
        memcpy(payload, opts->message, (size_t)payload_len);
    }

    mqtt_client_init(&c, transport, &co, txbuf, sizeof(txbuf), rxbuf,
                      sizeof(rxbuf));

    if (opts->verbose)
        printf("mqtt_pub: connecting to %s:%u\n", opts->host,
               (unsigned)opts->port);
    if (mqtt_client_connect(&c, tool_now_ms()) != 0) {
        fprintf(stderr, "mqtt_pub: connect failed\n");
        return 1;
    }

    deadline = tool_now_ms() + (uint32_t)opts->keepalive * 1000u + 5000u;
    while (mqtt_client_get_state(&c) == MQTT_CS_CONNECTING) {
        if (mqtt_client_process(&c, tool_now_ms(), NULL, NULL) != 0) {
            fprintf(stderr, "mqtt_pub: connect refused or timed out "
                            "(code %d, CONNACK %u)\n",
                    mqtt_client_last_error(&c),
                    (unsigned)mqtt_client_connack_code(&c));
            return 1;
        }
        if (tool_now_ms() > deadline) {
            fprintf(stderr, "mqtt_pub: timed out waiting for CONNACK\n");
            return 1;
        }
    }

    topic.ptr = opts->topic;
    topic.len = (uint16_t)strlen(opts->topic);
    if (opts->verbose)
        printf("mqtt_pub: publishing to %s (%ld bytes)\n", opts->topic,
               payload_len);
    if (mqtt_client_publish(&c, topic, payload, (size_t)payload_len,
                             opts->retain) != 0) {
        fprintf(stderr, "mqtt_pub: publish failed (code %d)\n",
                mqtt_client_last_error(&c));
        return 1;
    }

    mqtt_client_disconnect(&c);
    return 0;
}
