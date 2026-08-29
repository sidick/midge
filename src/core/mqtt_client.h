#ifndef MIDGE_MQTT_CLIENT_H
#define MIDGE_MQTT_CLIENT_H

/* A pump-driven MQTT 3.1.1 client: no timers, no sockets, no allocation.
 * The caller supplies a transport (src/core/mqtt_transport.h), tx/rx
 * buffers, and a clock (`now_ms`, any monotonic millisecond counter the
 * caller chooses - a wall clock, a free-running counter, whatever). Because
 * of that, this same struct/API is exercised by host unit tests against a
 * fake transport and fake clock, and later drops unchanged into the Phase 2
 * mqtt.library subprocess loop.
 *
 * Scope for this v1: QoS 0 publish, QoS 0/1 subscribe (an incoming QoS 1
 * PUBLISH is acknowledged automatically), keepalive PINGREQ scheduling and
 * timeout detection. QoS 1 *outbound* publish with retransmission and
 * reconnect/auto-resubscribe are Phase 2 (see docs/ARCHITECTURE.md); QoS 2
 * is out of scope entirely (docs/PROTOCOL.md). */

#include <stddef.h>
#include <stdint.h>

#include "mqtt_packet.h"
#include "mqtt_transport.h"

typedef enum {
    MQTT_CS_DISCONNECTED = 0,
    MQTT_CS_CONNECTING, /* CONNECT sent, waiting for CONNACK */
    MQTT_CS_CONNECTED,
    MQTT_CS_ERROR /* see mqtt_client_last_error(); call mqtt_client_init()
                     again (with a fresh transport) to retry */
} mqtt_client_state;

/* Client-level error codes, disjoint from mqtt_err (0..4) so a caller can
 * tell a codec-level failure (a negative mqtt_err, e.g. from a malformed
 * incoming packet) from one of these (mqtt_client_last_error() returns
 * whichever fired, always as a positive code - the sign is only used on
 * function return values). */
typedef enum {
    MQTT_CLIENT_ERR_TRANSPORT = 100, /* send()/recv() reported a fatal error */
    MQTT_CLIENT_ERR_CONNECT_TIMEOUT, /* no CONNACK within one keepalive period */
    MQTT_CLIENT_ERR_CONNECT_REFUSED, /* CONNACK arrived with a non-zero code;
                                         see mqtt_client_connack_code() */
    MQTT_CLIENT_ERR_KEEPALIVE_TIMEOUT, /* no PINGRESP within one keepalive period */
    MQTT_CLIENT_ERR_NOT_CONNECTED      /* publish/subscribe called out of state */
} mqtt_client_err;

typedef void (*mqtt_msg_cb)(void *user, const mqtt_packet *publish);

typedef struct {
    mqtt_transport *transport;
    mqtt_connect_opts opts;

    uint8_t *txbuf;
    size_t txcap;
    uint8_t *rxbuf;
    size_t rxcap;
    size_t rxlen; /* bytes currently buffered, awaiting a complete packet */

    mqtt_client_state state;
    int last_error;          /* a negative mqtt_err or mqtt_client_err; only
                                 meaningful when state == MQTT_CS_ERROR */
    uint8_t connack_code;     /* last CONNACK return code seen, if any */

    uint32_t keepalive_ms;
    uint32_t last_send_ms;    /* set by connect() and process()'s own PINGREQ;
                                  publish()/subscribe() do NOT update this
                                  (they take no clock argument - see below),
                                  so a keepalive PINGREQ may fire slightly
                                  earlier than strictly necessary, never later */
    uint32_t connect_sent_ms;
    int ping_outstanding;
    uint32_t ping_sent_ms;

    uint16_t next_packet_id;
} mqtt_client;

/* Wires up `c` against `transport` with connect options `opts` (copied by
 * value) and caller-owned tx/rx scratch buffers. `opts->keepalive` also
 * governs the CONNACK and PINGRESP timeouts (one keepalive period each).
 * Does not touch the transport - call mqtt_client_connect() next. */
void mqtt_client_init(mqtt_client *c, mqtt_transport *transport,
                       const mqtt_connect_opts *opts, uint8_t *txbuf,
                       size_t txcap, uint8_t *rxbuf, size_t rxcap);

/* Encodes and sends CONNECT, moving to MQTT_CS_CONNECTING. Returns 0, or a
 * negative mqtt_err/mqtt_client_err (also latched via last_error). */
int mqtt_client_connect(mqtt_client *c, uint32_t now_ms);

/* QoS 0 publish. Returns 0, or a negative mqtt_err/mqtt_client_err. */
int mqtt_client_publish(mqtt_client *c, mqtt_str topic,
                         const uint8_t *payload, size_t len, int retain);

/* Subscribes to a single filter at the given QoS (0 or 1). Returns 0, or a
 * negative mqtt_err/mqtt_client_err. */
int mqtt_client_subscribe(mqtt_client *c, mqtt_str filter, uint8_t qos);

/* Sends DISCONNECT (best-effort) and closes the transport. Leaves the
 * client in MQTT_CS_DISCONNECTED either way. */
void mqtt_client_disconnect(mqtt_client *c);

/* Drains available inbound bytes, advances the state machine (CONNACK,
 * PINGRESP, auto-PUBACK of incoming QoS 1 messages), invokes `cb` once per
 * PUBLISH delivered to the caller AND once per received acknowledgement-
 * class packet (PUBACK/PUBREC/PUBREL/PUBCOMP/SUBACK/UNSUBACK - core does no
 * bookkeeping of its own for these; a caller that wants to match them
 * against an outstanding QoS 1 publish or SUBSCRIBE must do so itself, e.g.
 * mqtt.library's subprocess, see docs/ARCHITECTURE.md), and handles
 * keepalive PINGREQ scheduling and CONNACK/PINGRESP timeout detection
 * against `now_ms`. Callers that only want deliveries must filter on
 * pkt->type == MQTT_PUBLISH themselves. Returns 0, or a negative
 * mqtt_err/mqtt_client_err (also latched via last_error and a transition to
 * MQTT_CS_ERROR). Safe to call from CONNECTING or CONNECTED state; a no-op
 * in DISCONNECTED/ERROR. */
int mqtt_client_process(mqtt_client *c, uint32_t now_ms, mqtt_msg_cb cb,
                         void *user);

/* Absolute `now_ms` timestamp at which the caller should next call
 * mqtt_client_process() even with no I/O ready (for a select()/WaitSelect()
 * timeout) - the next scheduled PINGREQ, or the current CONNACK/PINGRESP
 * timeout deadline. Returns 0 if there is no pending deadline (DISCONNECTED,
 * ERROR, or keepalive disabled via opts.keepalive == 0). */
uint32_t mqtt_client_next_deadline(const mqtt_client *c);

mqtt_client_state mqtt_client_get_state(const mqtt_client *c);
int mqtt_client_last_error(const mqtt_client *c);
uint8_t mqtt_client_connack_code(const mqtt_client *c);

#endif
