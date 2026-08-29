/* test_client.c — mqtt_client state machine, driven by a fake in-memory
 * transport and an explicit stepped clock (no real sockets, no real time). */
#include <string.h>

#include "test.h"
#include "mqtt_client.h"
#include "vectors.h"

/* --- Fake transport: captures everything sent, serves bytes from a
 * caller-loaded inbox for recv(). --- */
typedef struct {
    uint8_t sent[512];
    size_t sent_len;

    uint8_t inbox[512];
    size_t inbox_len;
    size_t inbox_pos;

    int closed;
    int err_when_drained; /* recv() returns -1 once the inbox is empty */
} fake_conn;

static int fake_send(void *ctx, const uint8_t *buf, size_t len)
{
    fake_conn *f = (fake_conn *)ctx;
    if (f->sent_len + len > sizeof(f->sent))
        return -1;
    memcpy(f->sent + f->sent_len, buf, len);
    f->sent_len += len;
    return (int)len;
}

static int fake_recv(void *ctx, uint8_t *buf, size_t cap)
{
    fake_conn *f = (fake_conn *)ctx;
    size_t avail = f->inbox_len - f->inbox_pos;
    if (avail == 0)
        return f->err_when_drained ? -1 : 0;
    if (avail > cap)
        avail = cap;
    memcpy(buf, f->inbox + f->inbox_pos, avail);
    f->inbox_pos += avail;
    return (int)avail;
}

static void fake_close(void *ctx)
{
    ((fake_conn *)ctx)->closed = 1;
}

static void fake_feed(fake_conn *f, const uint8_t *data, size_t len)
{
    memcpy(f->inbox + f->inbox_len, data, len);
    f->inbox_len += len;
}

static void connect_and_accept(mqtt_client *c, fake_conn *f, mqtt_transport *t,
                                uint8_t *txbuf, size_t txcap, uint8_t *rxbuf,
                                size_t rxcap, uint16_t keepalive_s)
{
    mqtt_connect_opts opts;

    memset(f, 0, sizeof(*f));
    t->ctx = f;
    t->send = fake_send;
    t->recv = fake_recv;
    t->close = fake_close;

    memset(&opts, 0, sizeof(opts));
    opts.client_id = MQTT_STR("t1");
    opts.clean_session = 1;
    opts.keepalive = keepalive_s;

    mqtt_client_init(c, t, &opts, txbuf, txcap, rxbuf, rxcap);
    TEST_CHECK(mqtt_client_connect(c, 1) == 0);

    fake_feed(f, V_CONNACK_OK, sizeof(V_CONNACK_OK));
    TEST_CHECK(mqtt_client_process(c, 1, NULL, NULL) == 0);
    TEST_CHECK(mqtt_client_get_state(c) == MQTT_CS_CONNECTED);
}

static void test_connect_happy_path(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];
    mqtt_connect_opts opts;

    memset(&f, 0, sizeof(f));
    t.ctx = &f; t.send = fake_send; t.recv = fake_recv; t.close = fake_close;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = MQTT_STR("mid1"); /* matches V_CONNECT for a byte-exact check */
    opts.clean_session = 1;
    opts.keepalive = 60;

    mqtt_client_init(&c, &t, &opts, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_DISCONNECTED);

    TEST_CHECK(mqtt_client_connect(&c, 0) == 0);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_CONNECTING);
    TEST_CHECK(f.sent_len == sizeof(V_CONNECT));
    TEST_CHECK(memcmp(f.sent, V_CONNECT, sizeof(V_CONNECT)) == 0);

    fake_feed(&f, V_CONNACK_OK, sizeof(V_CONNACK_OK));
    TEST_CHECK(mqtt_client_process(&c, 5, NULL, NULL) == 0);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_CONNECTED);
    TEST_CHECK(mqtt_client_connack_code(&c) == 0);
}

static void test_connect_refused(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];
    mqtt_connect_opts opts;

    memset(&f, 0, sizeof(f));
    t.ctx = &f; t.send = fake_send; t.recv = fake_recv; t.close = fake_close;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = MQTT_STR("t1");
    opts.keepalive = 60;

    mqtt_client_init(&c, &t, &opts, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
    TEST_CHECK(mqtt_client_connect(&c, 0) == 0);

    fake_feed(&f, V_CONNACK_REFUSED, sizeof(V_CONNACK_REFUSED));
    TEST_CHECK(mqtt_client_process(&c, 5, NULL, NULL) ==
               -MQTT_CLIENT_ERR_CONNECT_REFUSED);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_ERROR);
    TEST_CHECK(mqtt_client_connack_code(&c) == 5);
}

static void test_connect_timeout(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];
    mqtt_connect_opts opts;

    memset(&f, 0, sizeof(f));
    t.ctx = &f; t.send = fake_send; t.recv = fake_recv; t.close = fake_close;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = MQTT_STR("t1");
    opts.keepalive = 10; /* 10s -> 10000ms timeout */

    mqtt_client_init(&c, &t, &opts, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
    TEST_CHECK(mqtt_client_connect(&c, 1000) == 0);

    /* No CONNACK arrives. Just under the deadline: still waiting. */
    TEST_CHECK(mqtt_client_process(&c, 1000 + 9999, NULL, NULL) == 0);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_CONNECTING);

    /* At the deadline: timeout. */
    TEST_CHECK(mqtt_client_process(&c, 1000 + 10000, NULL, NULL) ==
               -MQTT_CLIENT_ERR_CONNECT_TIMEOUT);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_ERROR);
}

/* keepalive == 0 legally disables keepalive (MQTT 3.1.1); it must not also
 * make the CONNACK wait time out immediately, and there must be no pending
 * deadline to report while CONNECTING with it disabled. */
static void test_connect_with_keepalive_disabled(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];
    mqtt_connect_opts opts;

    memset(&f, 0, sizeof(f));
    t.ctx = &f; t.send = fake_send; t.recv = fake_recv; t.close = fake_close;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = MQTT_STR("t1");
    opts.keepalive = 0;

    mqtt_client_init(&c, &t, &opts, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
    TEST_CHECK(mqtt_client_connect(&c, 1000) == 0);
    TEST_CHECK(mqtt_client_next_deadline(&c) == 0);

    /* Long after what a keepalive-timeout would have been, still waiting -
     * no bogus instant timeout. */
    TEST_CHECK(mqtt_client_process(&c, 1000 + 60000, NULL, NULL) == 0);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_CONNECTING);

    fake_feed(&f, V_CONNACK_OK, sizeof(V_CONNACK_OK));
    TEST_CHECK(mqtt_client_process(&c, 1000 + 60000, NULL, NULL) == 0);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_CONNECTED);
    TEST_CHECK(mqtt_client_next_deadline(&c) == 0);
}

/* A broker that sends a refusing CONNACK and then closes the connection
 * must be reported as CONNECT_REFUSED even though the close is what the
 * next recv() call actually observes. */
static void test_connect_refused_then_transport_error(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];
    mqtt_connect_opts opts;

    memset(&f, 0, sizeof(f));
    t.ctx = &f; t.send = fake_send; t.recv = fake_recv; t.close = fake_close;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = MQTT_STR("t1");
    opts.keepalive = 60;

    mqtt_client_init(&c, &t, &opts, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
    TEST_CHECK(mqtt_client_connect(&c, 0) == 0);

    fake_feed(&f, V_CONNACK_REFUSED, sizeof(V_CONNACK_REFUSED));
    /* Once the inbox (the CONNACK) is drained, simulate the broker's close
     * racing the refusal: the next recv() reports a transport error. */
    f.err_when_drained = 1;

    TEST_CHECK(mqtt_client_process(&c, 5, NULL, NULL) ==
               -MQTT_CLIENT_ERR_CONNECT_REFUSED);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_ERROR);
    TEST_CHECK(mqtt_client_connack_code(&c) == 5);
}

static void test_partial_packet_reassembly(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];
    mqtt_connect_opts opts;

    memset(&f, 0, sizeof(f));
    t.ctx = &f; t.send = fake_send; t.recv = fake_recv; t.close = fake_close;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = MQTT_STR("t1");
    opts.keepalive = 60;

    mqtt_client_init(&c, &t, &opts, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));
    TEST_CHECK(mqtt_client_connect(&c, 0) == 0);

    /* Feed the CONNACK one byte at a time; state must not flip to
     * CONNECTED until the last byte arrives. */
    {
        size_t i;
        for (i = 0; i < sizeof(V_CONNACK_OK) - 1; i++) {
            fake_feed(&f, V_CONNACK_OK + i, 1);
            TEST_CHECK(mqtt_client_process(&c, 1, NULL, NULL) == 0);
            TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_CONNECTING);
        }
        fake_feed(&f, V_CONNACK_OK + i, 1);
        TEST_CHECK(mqtt_client_process(&c, 1, NULL, NULL) == 0);
        TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_CONNECTED);
    }
}

static void test_keepalive_ping_and_timeout(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];

    connect_and_accept(&c, &f, &t, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf), 8);
    /* keepalive_ms = 8000; ping scheduled at last_send_ms + 6000. */

    f.sent_len = 0; /* only care about bytes sent from here on */

    /* Just before the 3/4 mark: no ping yet. */
    TEST_CHECK(mqtt_client_process(&c, 1 + 5999, NULL, NULL) == 0);
    TEST_CHECK(f.sent_len == 0);

    /* At the mark: PINGREQ goes out. */
    TEST_CHECK(mqtt_client_process(&c, 1 + 6000, NULL, NULL) == 0);
    TEST_CHECK(f.sent_len == sizeof(V_PINGREQ));
    TEST_CHECK(memcmp(f.sent, V_PINGREQ, sizeof(V_PINGREQ)) == 0);

    /* PINGRESP arrives: outstanding flag clears, no error. */
    fake_feed(&f, V_PINGRESP, sizeof(V_PINGRESP));
    TEST_CHECK(mqtt_client_process(&c, 1 + 6001, NULL, NULL) == 0);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_CONNECTED);
}

static void test_keepalive_timeout_without_pingresp(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];

    connect_and_accept(&c, &f, &t, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf), 8);
    f.sent_len = 0;

    TEST_CHECK(mqtt_client_process(&c, 1 + 6000, NULL, NULL) == 0);
    TEST_CHECK(f.sent_len == sizeof(V_PINGREQ)); /* PINGREQ sent */

    /* No PINGRESP ever arrives; one keepalive period after the ping. */
    TEST_CHECK(mqtt_client_process(&c, 1 + 6000 + 7999, NULL, NULL) == 0);
    TEST_CHECK(mqtt_client_process(&c, 1 + 6000 + 8000, NULL, NULL) ==
               -MQTT_CLIENT_ERR_KEEPALIVE_TIMEOUT);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_ERROR);
}

static void test_publish_and_subscribe_require_connected(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];
    mqtt_connect_opts opts;

    memset(&f, 0, sizeof(f));
    t.ctx = &f; t.send = fake_send; t.recv = fake_recv; t.close = fake_close;
    memset(&opts, 0, sizeof(opts));
    opts.client_id = MQTT_STR("t1");
    opts.keepalive = 60;
    mqtt_client_init(&c, &t, &opts, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf));

    TEST_CHECK(mqtt_client_publish(&c, MQTT_STR("t"), (const uint8_t *)"x", 1,
                                    0) == -MQTT_CLIENT_ERR_NOT_CONNECTED);
    TEST_CHECK(mqtt_client_subscribe(&c, MQTT_STR("t"), 0) ==
               -MQTT_CLIENT_ERR_NOT_CONNECTED);
}

static void test_subscribe_encodes_incrementing_packet_ids(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];

    connect_and_accept(&c, &f, &t, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf), 60);
    f.sent_len = 0;

    TEST_CHECK(mqtt_client_subscribe(&c, MQTT_STR("a/b"), 1) == 0);
    /* packet id 1, big-endian, right after the 2-byte fixed header. */
    TEST_CHECK(f.sent[2] == 0x00 && f.sent[3] == 0x01);

    f.sent_len = 0;
    TEST_CHECK(mqtt_client_subscribe(&c, MQTT_STR("c/d"), 0) == 0);
    TEST_CHECK(f.sent[2] == 0x00 && f.sent[3] == 0x02);
}

static int g_cb_calls;
static char g_cb_topic[32];
static uint8_t g_cb_payload[32];
static uint32_t g_cb_payload_len;

static void record_publish(void *user, const mqtt_packet *pkt)
{
    (void)user;
    g_cb_calls++;
    memcpy(g_cb_topic, pkt->u.publish.topic.ptr, pkt->u.publish.topic.len);
    g_cb_topic[pkt->u.publish.topic.len] = '\0';
    g_cb_payload_len = pkt->u.publish.payload_len;
    memcpy(g_cb_payload, pkt->u.publish.payload, g_cb_payload_len);
}

static void test_publish_delivery_and_qos1_autoack(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];

    connect_and_accept(&c, &f, &t, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf), 60);

    g_cb_calls = 0;
    fake_feed(&f, V_PUBLISH_QOS0, sizeof(V_PUBLISH_QOS0));
    TEST_CHECK(mqtt_client_process(&c, 2, record_publish, NULL) == 0);
    TEST_CHECK(g_cb_calls == 1);
    TEST_CHECK(strcmp(g_cb_topic, "t") == 0);
    TEST_CHECK(g_cb_payload_len == 2 && memcmp(g_cb_payload, "hi", 2) == 0);

    /* An incoming QoS 1 PUBLISH is delivered AND auto-acknowledged. */
    f.sent_len = 0;
    fake_feed(&f, V_PUBLISH_QOS1, sizeof(V_PUBLISH_QOS1));
    TEST_CHECK(mqtt_client_process(&c, 3, record_publish, NULL) == 0);
    TEST_CHECK(g_cb_calls == 2);
    TEST_CHECK(f.sent_len == sizeof(V_PUBACK));
    TEST_CHECK(memcmp(f.sent, V_PUBACK, sizeof(V_PUBACK)) == 0);
}

static int g_ack_calls;
static uint8_t g_ack_type;
static uint16_t g_ack_id;
static uint8_t g_ack_suback_count;
static uint8_t g_ack_suback_code;

static void record_acks(void *user, const mqtt_packet *pkt)
{
    (void)user;
    if (pkt->type == MQTT_PUBLISH)
        return;
    g_ack_calls++;
    g_ack_type = pkt->type;
    if (pkt->type == MQTT_SUBACK) {
        g_ack_id = pkt->u.suback.packet_id;
        g_ack_suback_count = (uint8_t)pkt->u.suback.count;
        g_ack_suback_code = pkt->u.suback.codes[0];
    } else {
        g_ack_id = pkt->u.ack.packet_id;
    }
}

/* PUBACK/SUBACK now surface through cb (with correct type + packet id) so a
 * caller layered above core (mqtt.library's subprocess) can implement
 * QoS 1 publish-with-retransmit and SUBACK matching without core itself
 * tracking any of that state - see mqtt_client.h's process() doc comment. */
static void test_ack_packets_surface_through_cb(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];

    connect_and_accept(&c, &f, &t, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf), 60);

    g_ack_calls = 0;
    fake_feed(&f, V_PUBACK, sizeof(V_PUBACK));
    TEST_CHECK(mqtt_client_process(&c, 2, record_acks, NULL) == 0);
    TEST_CHECK(g_ack_calls == 1);
    TEST_CHECK(g_ack_type == MQTT_PUBACK);
    TEST_CHECK(g_ack_id == 0x0007);

    g_ack_calls = 0;
    fake_feed(&f, V_SUBACK, sizeof(V_SUBACK));
    TEST_CHECK(mqtt_client_process(&c, 3, record_acks, NULL) == 0);
    TEST_CHECK(g_ack_calls == 1);
    TEST_CHECK(g_ack_type == MQTT_SUBACK);
    TEST_CHECK(g_ack_id == 0x000A);
    TEST_CHECK(g_ack_suback_count == 1);
    TEST_CHECK(g_ack_suback_code == 0x01);

    /* Existing PUBLISH delivery behaviour is unchanged: a PUBLISH still
     * reaches a callback that only looks at pkt->type == MQTT_PUBLISH, and
     * does NOT bump the ack counter. */
    g_cb_calls = 0;
    g_ack_calls = 0;
    fake_feed(&f, V_PUBLISH_QOS0, sizeof(V_PUBLISH_QOS0));
    TEST_CHECK(mqtt_client_process(&c, 4, record_publish, NULL) == 0);
    TEST_CHECK(g_cb_calls == 1);
    TEST_CHECK(g_ack_calls == 0);
}

static void test_disconnect_sends_disconnect_and_closes(void)
{
    fake_conn f;
    mqtt_transport t;
    mqtt_client c;
    uint8_t txbuf[128], rxbuf[128];

    connect_and_accept(&c, &f, &t, txbuf, sizeof(txbuf), rxbuf, sizeof(rxbuf), 60);
    f.sent_len = 0;

    mqtt_client_disconnect(&c);
    TEST_CHECK(f.sent_len == sizeof(V_DISCONNECT));
    TEST_CHECK(memcmp(f.sent, V_DISCONNECT, sizeof(V_DISCONNECT)) == 0);
    TEST_CHECK(f.closed == 1);
    TEST_CHECK(mqtt_client_get_state(&c) == MQTT_CS_DISCONNECTED);
}

void run_client_tests(void)
{
    test_connect_happy_path();
    test_connect_refused();
    test_connect_timeout();
    test_connect_with_keepalive_disabled();
    test_connect_refused_then_transport_error();
    test_partial_packet_reassembly();
    test_keepalive_ping_and_timeout();
    test_keepalive_timeout_without_pingresp();
    test_publish_and_subscribe_require_connected();
    test_subscribe_encodes_incrementing_packet_ids();
    test_publish_delivery_and_qos1_autoack();
    test_ack_packets_surface_through_cb();
    test_disconnect_sends_disconnect_and_closes();
}
