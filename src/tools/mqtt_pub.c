#include "tool_opts.h"

#include <stdio.h>
#include <string.h>

#include "mqtt_client.h"
#include "tool_clock.h"

/* Default max packet size (proposal: 8KB covers smart-home payloads) plus
 * headroom for the fixed header - see mqtt_packet.h's note on `buf` sizing. */
#define MQTT_TOOL_BUF_SIZE 8320

/* Returns the file's length on success, or -1 on failure with *toobig set
 * when the file is larger than `cap` (refusing a silent truncation).
 * fread() filling
 * `buf` to exactly `cap` bytes does NOT itself set the stream's EOF flag -
 * that only happens once a read is attempted past the actual end of file -
 * so an exactly-`cap`-byte file must not be mistaken for "too big"; probe
 * one more byte with fgetc() to tell the two cases apart. */
static long read_file(const char *path, uint8_t *buf, size_t cap, int *toobig)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    *toobig = 0;
    if (!f)
        return -1;
    n = fread(buf, 1, cap, f);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    if (n == cap && fgetc(f) != EOF) {
        fclose(f);
        *toobig = 1;
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
        transport->close(transport->ctx);
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
        int toobig;

        payload_len = read_file(opts->file, payload, sizeof(payload), &toobig);
        if (payload_len < 0) {
            if (toobig)
                fprintf(stderr, "mqtt_pub: %s exceeds %d bytes\n",
                        opts->file, MQTT_TOOL_BUF_SIZE);
            else
                fprintf(stderr, "mqtt_pub: cannot read %s\n", opts->file);
            transport->close(transport->ctx);
            return 1;
        }
    } else if (opts->message) {
        payload_len = (long)strlen(opts->message);
        if ((size_t)payload_len > sizeof(payload)) {
            fprintf(stderr, "mqtt_pub: message exceeds %d bytes\n",
                    MQTT_TOOL_BUF_SIZE);
            transport->close(transport->ctx);
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
        transport->close(transport->ctx);
        return 1;
    }

    deadline = tool_now_ms() + (uint32_t)opts->keepalive * 1000u + 5000u;
    while (mqtt_client_get_state(&c) == MQTT_CS_CONNECTING) {
        if (mqtt_client_process(&c, tool_now_ms(), NULL, NULL) != 0) {
            fprintf(stderr, "mqtt_pub: connect refused or timed out "
                            "(code %d, CONNACK %u)\n",
                    mqtt_client_last_error(&c),
                    (unsigned)mqtt_client_connack_code(&c));
            transport->close(transport->ctx);
            return 1;
        }
        /* Wrap-safe elapsed-time compare (now - deadline as a signed
         * difference of the unsigned subtraction), matching src/core's own
         * keepalive arithmetic - a plain `now > deadline` misbehaves across
         * tool_now_ms()'s ~49.7-day uint32 wrap. */
        if ((int32_t)(tool_now_ms() - deadline) >= 0) {
            fprintf(stderr, "mqtt_pub: timed out waiting for CONNACK\n");
            transport->close(transport->ctx);
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
        transport->close(transport->ctx);
        return 1;
    }

    mqtt_client_disconnect(&c);
    return 0;
}
