#include "tool_opts.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_client.h"
#include "tool_clock.h"

#define MQTT_TOOL_BUF_SIZE 8320

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

typedef struct {
    int limit;
    int seen;
    int verbose;
} sub_ctx;

static void on_publish(void *user, const mqtt_packet *pkt)
{
    sub_ctx *s = (sub_ctx *)user;

    if (pkt->type != MQTT_PUBLISH)
        return; /* core's cb also fires for ack-class packets now - this
                    tool only ever cared about deliveries */

    if (s->verbose)
        printf("%.*s ", (int)pkt->u.publish.topic.len, pkt->u.publish.topic.ptr);
    fwrite(pkt->u.publish.payload, 1, pkt->u.publish.payload_len, stdout);
    printf("\n");
    fflush(stdout);
    s->seen++;
}

int mqtt_sub_run(const tool_opts *opts, mqtt_transport *transport)
{
    static uint8_t txbuf[MQTT_TOOL_BUF_SIZE];
    static uint8_t rxbuf[MQTT_TOOL_BUF_SIZE];
    mqtt_connect_opts co;
    mqtt_client c;
    mqtt_str filter;
    sub_ctx sctx;
    uint32_t deadline;

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

    /* Ctrl-C stops the receive loop cleanly (flushes/disconnects below)
     * rather than killing the process mid-packet. */
    signal(SIGINT, on_sigint);

    mqtt_client_init(&c, transport, &co, txbuf, sizeof(txbuf), rxbuf,
                      sizeof(rxbuf));

    if (opts->verbose)
        printf("mqtt_sub: connecting to %s:%u\n", opts->host,
               (unsigned)opts->port);
    if (mqtt_client_connect(&c, tool_now_ms()) != 0) {
        fprintf(stderr, "mqtt_sub: connect failed\n");
        return 1;
    }

    deadline = tool_now_ms() + (uint32_t)opts->keepalive * 1000u + 5000u;
    while (mqtt_client_get_state(&c) == MQTT_CS_CONNECTING) {
        if (mqtt_client_process(&c, tool_now_ms(), NULL, NULL) != 0) {
            fprintf(stderr, "mqtt_sub: connect refused or timed out "
                            "(code %d, CONNACK %u)\n",
                    mqtt_client_last_error(&c),
                    (unsigned)mqtt_client_connack_code(&c));
            return 1;
        }
        if (tool_now_ms() > deadline) {
            fprintf(stderr, "mqtt_sub: timed out waiting for CONNACK\n");
            return 1;
        }
    }

    filter.ptr = opts->topic;
    filter.len = (uint16_t)strlen(opts->topic);
    if (mqtt_client_subscribe(&c, filter, opts->qos) != 0) {
        fprintf(stderr, "mqtt_sub: subscribe failed (code %d)\n",
                mqtt_client_last_error(&c));
        return 1;
    }
    if (opts->verbose)
        printf("mqtt_sub: subscribed to %s (QoS %u)\n", opts->topic,
               (unsigned)opts->qos);

    sctx.limit = opts->count;
    sctx.seen = 0;
    sctx.verbose = opts->verbose;

    while (!g_stop && mqtt_client_get_state(&c) == MQTT_CS_CONNECTED) {
        if (mqtt_client_process(&c, tool_now_ms(), on_publish, &sctx) != 0) {
            fprintf(stderr, "mqtt_sub: connection lost (code %d)\n",
                    mqtt_client_last_error(&c));
            return 1;
        }
        if (sctx.limit > 0 && sctx.seen >= sctx.limit)
            break;
    }

    mqtt_client_disconnect(&c);
    return 0;
}
