#include "mqtt_client.h"

#include <string.h>

/* Safety valve for a transport that persistently would-block inside a
 * single connect()/publish()/subscribe() call: not a substitute for a real
 * non-blocking event loop (that's what mqtt_client_process()'s caller-driven
 * pumping is for), just a guard against spinning forever in this v1 client,
 * whose CLI callers (mqtt_pub/mqtt_sub) use blocking-mode sockets. */
#define MQTT_SEND_MAX_SPINS 1000

static uint16_t alloc_packet_id(mqtt_client *c)
{
    uint16_t id = c->next_packet_id;
    c->next_packet_id = (uint16_t)(id == 0xFFFF ? 1 : id + 1);
    return id;
}

static int send_all(mqtt_client *c, int len)
{
    size_t sent = 0;
    int spins = 0;

    while (sent < (size_t)len) {
        int n = c->transport->send(c->transport->ctx, c->txbuf + sent,
                                    (size_t)len - sent);
        if (n < 0)
            return -MQTT_CLIENT_ERR_TRANSPORT;
        if (n == 0) {
            if (++spins > MQTT_SEND_MAX_SPINS)
                return -MQTT_CLIENT_ERR_TRANSPORT;
            continue;
        }
        sent += (size_t)n;
        spins = 0;
    }
    return 0;
}

static int fail(mqtt_client *c, int err)
{
    c->state = MQTT_CS_ERROR;
    c->last_error = -err;
    return -err;
}

void mqtt_client_init(mqtt_client *c, mqtt_transport *transport,
                       const mqtt_connect_opts *opts, uint8_t *txbuf,
                       size_t txcap, uint8_t *rxbuf, size_t rxcap)
{
    memset(c, 0, sizeof(*c));
    c->transport = transport;
    c->opts = *opts;
    c->txbuf = txbuf;
    c->txcap = txcap;
    c->rxbuf = rxbuf;
    c->rxcap = rxcap;
    c->state = MQTT_CS_DISCONNECTED;
    c->keepalive_ms = (uint32_t)opts->keepalive * 1000u;
    c->next_packet_id = 1;
}

int mqtt_client_connect(mqtt_client *c, uint32_t now_ms)
{
    int n = mqtt_encode_connect(c->txbuf, c->txcap, &c->opts);
    int rc;

    if (n < 0)
        return fail(c, -n);
    rc = send_all(c, n);
    if (rc < 0)
        return fail(c, -rc);

    c->state = MQTT_CS_CONNECTING;
    c->last_send_ms = now_ms;
    c->connect_sent_ms = now_ms;
    c->ping_outstanding = 0;
    return 0;
}

int mqtt_client_publish(mqtt_client *c, mqtt_str topic,
                         const uint8_t *payload, size_t len, int retain)
{
    int n, rc;

    if (c->state != MQTT_CS_CONNECTED)
        return -MQTT_CLIENT_ERR_NOT_CONNECTED;

    n = mqtt_encode_publish(c->txbuf, c->txcap, topic, payload, len,
                             0 /* QoS 0 - see mqtt_client.h scope note */,
                             retain, 0, 0);
    if (n < 0)
        return fail(c, -n);
    rc = send_all(c, n);
    if (rc < 0)
        return fail(c, -rc);
    return 0;
}

int mqtt_client_subscribe(mqtt_client *c, mqtt_str filter, uint8_t qos)
{
    int n, rc;
    uint16_t id;

    if (c->state != MQTT_CS_CONNECTED)
        return -MQTT_CLIENT_ERR_NOT_CONNECTED;

    id = alloc_packet_id(c);
    n = mqtt_encode_subscribe(c->txbuf, c->txcap, id, &filter, &qos, 1);
    if (n < 0)
        return fail(c, -n);
    rc = send_all(c, n);
    if (rc < 0)
        return fail(c, -rc);
    c->last_subscribe_id = id;
    return 0;
}

uint16_t mqtt_client_last_subscribe_id(const mqtt_client *c)
{
    return c->last_subscribe_id;
}

void mqtt_client_disconnect(mqtt_client *c)
{
    if (c->state == MQTT_CS_CONNECTING || c->state == MQTT_CS_CONNECTED) {
        int n = mqtt_encode_disconnect(c->txbuf, c->txcap);
        if (n > 0)
            send_all(c, n); /* best-effort */
    }
    if (c->transport && c->transport->close)
        c->transport->close(c->transport->ctx);
    c->state = MQTT_CS_DISCONNECTED;
}

static void handle_packet(mqtt_client *c, const mqtt_packet *pkt,
                           mqtt_msg_cb cb, void *user)
{
    switch (pkt->type) {
    case MQTT_CONNACK:
        c->connack_code = pkt->u.connack.return_code;
        if (c->state == MQTT_CS_CONNECTING) {
            if (pkt->u.connack.return_code == 0)
                c->state = MQTT_CS_CONNECTED;
            /* else: leave state alone here - process() reports the refusal
             * as its return value; see the CONNACK handling below it. */
        }
        break;

    case MQTT_PINGRESP:
        c->ping_outstanding = 0;
        break;

    case MQTT_PUBLISH:
        if (pkt->u.publish.qos == 1) {
            int n = mqtt_encode_puback(c->txbuf, c->txcap,
                                        pkt->u.publish.packet_id);
            if (n > 0)
                send_all(c, n); /* best-effort; a real failure surfaces via
                                    the next process() call's own recv/send */
        }
        if (cb)
            cb(user, pkt);
        break;

    default:
        /* PUBACK/PUBREC/PUBREL/PUBCOMP/SUBACK/UNSUBACK: no bookkeeping here
         * (no outbound QoS 1 state, no subscribe-state tracking in core -
         * that lives in the mqtt.library subprocess, see
         * docs/ARCHITECTURE.md). Still surfaced to the caller's callback so
         * it can observe acknowledgements (e.g. to implement QoS 1
         * publish-with-retransmit or SUBACK matching above this layer);
         * callers that only care about deliveries must filter on
         * pkt->type == MQTT_PUBLISH themselves. */
        if (cb)
            cb(user, pkt);
        break;
    }
}

int mqtt_client_process(mqtt_client *c, uint32_t now_ms, mqtt_msg_cb cb,
                         void *user)
{
    if (c->state == MQTT_CS_DISCONNECTED || c->state == MQTT_CS_ERROR)
        return 0;

    if (c->state == MQTT_CS_CONNECTING) {
        /* keepalive == 0 legitimately disables the keepalive timer
         * (MQTT 3.1.1); it must not also make the CONNACK wait time out
         * immediately. */
        if (c->keepalive_ms > 0 &&
            now_ms - c->connect_sent_ms >= c->keepalive_ms)
            return fail(c, MQTT_CLIENT_ERR_CONNECT_TIMEOUT);
    } else if (c->state == MQTT_CS_CONNECTED && c->keepalive_ms > 0) {
        if (c->ping_outstanding) {
            if (now_ms - c->ping_sent_ms >= c->keepalive_ms)
                return fail(c, MQTT_CLIENT_ERR_KEEPALIVE_TIMEOUT);
        } else if (now_ms - c->last_send_ms >= (c->keepalive_ms * 3) / 4) {
            int n = mqtt_encode_pingreq(c->txbuf, c->txcap);
            int rc;
            if (n < 0)
                return fail(c, -n);
            rc = send_all(c, n);
            if (rc < 0)
                return fail(c, -rc);
            c->ping_outstanding = 1;
            c->ping_sent_ms = now_ms;
            c->last_send_ms = now_ms;
        }
    }

    for (;;) {
        int got = c->transport->recv(c->transport->ctx, c->rxbuf + c->rxlen,
                                      c->rxcap - c->rxlen);
        if (got < 0) {
            /* A broker that refuses the connection and then closes must be
             * reported as CONNECT_REFUSED, not a generic transport error,
             * even though the close is what the next recv() sees. */
            if (c->state == MQTT_CS_CONNECTING && c->connack_code != 0)
                return fail(c, MQTT_CLIENT_ERR_CONNECT_REFUSED);
            return fail(c, MQTT_CLIENT_ERR_TRANSPORT);
        }
        if (got == 0)
            break;
        c->rxlen += (size_t)got;

        for (;;) {
            mqtt_packet pkt;
            int used = mqtt_decode(c->rxbuf, c->rxlen, &pkt);
            if (used == 0)
                break;
            if (used < 0)
                return fail(c, -used);

            handle_packet(c, &pkt, cb, user);
            if (c->state == MQTT_CS_ERROR)
                return c->last_error;

            memmove(c->rxbuf, c->rxbuf + used, c->rxlen - (size_t)used);
            c->rxlen -= (size_t)used;
        }

        if (c->rxlen == c->rxcap)
            return fail(c, MQTT_ERR_BUFSIZE);
    }

    if (c->connack_code != 0 && c->state == MQTT_CS_CONNECTING)
        return fail(c, MQTT_CLIENT_ERR_CONNECT_REFUSED);

    return 0;
}

uint32_t mqtt_client_next_deadline(const mqtt_client *c)
{
    uint32_t deadline;

    if (c->state == MQTT_CS_CONNECTING) {
        /* keepalive == 0 disables the CONNACK-wait timeout too (see
         * mqtt_client_process()); no deadline to report. */
        if (c->keepalive_ms == 0)
            return 0;
        deadline = c->connect_sent_ms + c->keepalive_ms;
    } else if (c->state != MQTT_CS_CONNECTED || c->keepalive_ms == 0) {
        return 0;
    } else if (c->ping_outstanding) {
        deadline = c->ping_sent_ms + c->keepalive_ms;
    } else {
        deadline = c->last_send_ms + (c->keepalive_ms * 3) / 4;
    }

    /* 0 doubles as "no pending deadline"; a computed deadline that happens
     * to land exactly on the wrapped-clock value 0 must not be mistaken
     * for that sentinel. 1ms early is harmless (the caller just wakes up
     * a tick sooner than strictly required). */
    return deadline == 0 ? 1 : deadline;
}

mqtt_client_state mqtt_client_get_state(const mqtt_client *c)
{
    return c->state;
}

int mqtt_client_last_error(const mqtt_client *c)
{
    return c->last_error;
}

uint8_t mqtt_client_connack_code(const mqtt_client *c)
{
    return c->connack_code;
}
