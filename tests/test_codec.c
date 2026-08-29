/* test_codec.c — MQTT 3.1.1 packet codec: vectors, round-trips, truncation
 * sweeps, and malformed-input handling. */
#include <string.h>

#include "test.h"
#include "mqtt_packet.h"
#include "vectors.h"

static void check_bytes(const uint8_t *got, int got_len,
                         const uint8_t *want, size_t want_len)
{
    TEST_CHECK(got_len == (int)want_len);
    if (got_len == (int)want_len)
        TEST_CHECK(memcmp(got, want, want_len) == 0);
}

/* Every proper prefix of a complete packet must be reported incomplete
 * (never a false decode, never an out-of-bounds read - run under a
 * sanitizer this is the check that would catch it). */
static void check_truncation_sweep(const uint8_t *vec, size_t len)
{
    size_t i;
    mqtt_packet pkt;

    for (i = 0; i < len; i++)
        TEST_CHECK(mqtt_decode(vec, i, &pkt) == 0);
}

static void test_remlen(void)
{
    uint8_t buf[8];
    uint32_t v;
    int n;

    /* Boundary values from spec table 2.4. */
    TEST_CHECK(mqtt_remlen_encode(buf, 0) == 1 && buf[0] == 0x00);
    TEST_CHECK(mqtt_remlen_encode(buf, 127) == 1 && buf[0] == 0x7F);

    n = mqtt_remlen_encode(buf, 128);
    TEST_CHECK(n == 2 && buf[0] == 0x80 && buf[1] == 0x01);

    n = mqtt_remlen_encode(buf, 16383);
    TEST_CHECK(n == 2 && buf[0] == 0xFF && buf[1] == 0x7F);

    n = mqtt_remlen_encode(buf, 16384);
    TEST_CHECK(n == 3 && buf[0] == 0x80 && buf[1] == 0x80 && buf[2] == 0x01);

    n = mqtt_remlen_encode(buf, 2097151);
    TEST_CHECK(n == 3 && buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0x7F);

    n = mqtt_remlen_encode(buf, 268435455);
    TEST_CHECK(n == 4 && buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF &&
               buf[3] == 0x7F);

    TEST_CHECK(mqtt_remlen_encode(buf, 268435456UL) == -MQTT_ERR_PROTOCOL);

    /* Round-trip every boundary. */
    {
        static const uint32_t vals[] = { 0, 1, 127, 128, 16383, 16384,
                                          2097151, 2097152, 268435455 };
        size_t i;
        for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            n = mqtt_remlen_encode(buf, vals[i]);
            TEST_CHECK(n > 0);
            TEST_CHECK(mqtt_remlen_decode(buf, (size_t)n, &v) == n);
            TEST_CHECK(v == vals[i]);
        }
    }

    /* Incomplete: continuation bit set, not enough bytes supplied yet. */
    buf[0] = 0x80;
    TEST_CHECK(mqtt_remlen_decode(buf, 1, &v) == 0);
    TEST_CHECK(mqtt_remlen_decode(buf, 0, &v) == 0);

    /* Malformed: continuation bit still set after the 4th byte. */
    buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0xFF;
    TEST_CHECK(mqtt_remlen_decode(buf, 4, &v) == -MQTT_ERR_MALFORMED);
}

static void test_encode_vectors(void)
{
    uint8_t buf[64];
    int n;

    mqtt_connect_opts co;
    memset(&co, 0, sizeof(co));
    co.client_id = MQTT_STR("mid1");
    co.clean_session = 1;
    co.keepalive = 60;
    n = mqtt_encode_connect(buf, sizeof(buf), &co);
    check_bytes(buf, n, V_CONNECT, sizeof(V_CONNECT));

    n = mqtt_encode_publish(buf, sizeof(buf), MQTT_STR("t"),
                             (const uint8_t *)"hi", 2, 0, 0, 0, 0);
    check_bytes(buf, n, V_PUBLISH_QOS0, sizeof(V_PUBLISH_QOS0));

    n = mqtt_encode_publish(buf, sizeof(buf), MQTT_STR("t"),
                             (const uint8_t *)"hi", 2, 1, 1, 0, 7);
    check_bytes(buf, n, V_PUBLISH_QOS1, sizeof(V_PUBLISH_QOS1));

    n = mqtt_encode_puback(buf, sizeof(buf), 7);
    check_bytes(buf, n, V_PUBACK, sizeof(V_PUBACK));

    n = mqtt_encode_pubrec(buf, sizeof(buf), 7);
    check_bytes(buf, n, V_PUBREC, sizeof(V_PUBREC));

    n = mqtt_encode_pubrel(buf, sizeof(buf), 7);
    check_bytes(buf, n, V_PUBREL, sizeof(V_PUBREL));

    n = mqtt_encode_pubcomp(buf, sizeof(buf), 7);
    check_bytes(buf, n, V_PUBCOMP, sizeof(V_PUBCOMP));

    {
        mqtt_str topics[1] = { MQTT_STR("a/b") };
        uint8_t qos[1] = { 1 };
        n = mqtt_encode_subscribe(buf, sizeof(buf), 10, topics, qos, 1);
        check_bytes(buf, n, V_SUBSCRIBE, sizeof(V_SUBSCRIBE));
    }

    {
        mqtt_str topics[1] = { MQTT_STR("a/b") };
        n = mqtt_encode_unsubscribe(buf, sizeof(buf), 11, topics, 1);
        check_bytes(buf, n, V_UNSUBSCRIBE, sizeof(V_UNSUBSCRIBE));
    }

    n = mqtt_encode_pingreq(buf, sizeof(buf));
    check_bytes(buf, n, V_PINGREQ, sizeof(V_PINGREQ));

    n = mqtt_encode_disconnect(buf, sizeof(buf));
    check_bytes(buf, n, V_DISCONNECT, sizeof(V_DISCONNECT));
}

static void test_encode_bufsize(void)
{
    uint8_t tiny[1];
    uint8_t small[3];

    /* PINGREQ's 2-byte packet has no variable header/payload, so it needs
     * no offset-5 headroom (see the mqtt_packet.h note on `buf` sizing) -
     * only a buffer genuinely smaller than the packet itself fails. */
    TEST_CHECK(mqtt_encode_pingreq(tiny, sizeof(tiny)) == -MQTT_ERR_BUFSIZE);
    TEST_CHECK(mqtt_encode_pingreq(small, sizeof(small)) == 2);

    TEST_CHECK(mqtt_encode_puback(small, sizeof(small), 1) == -MQTT_ERR_BUFSIZE);
    TEST_CHECK(mqtt_encode_publish(small, sizeof(small), MQTT_STR("t"),
                                    (const uint8_t *)"hi", 2, 0, 0, 0, 0) ==
               -MQTT_ERR_BUFSIZE);

    /* Regression: cap < 5 (the offset-5 headroom w_init() starts writing
     * content at) used to underflow `cap - pos` in w_bytes() and let the
     * encoder memcpy past the end of `buf` before finish_packet() got a
     * chance to reject it. cap in [0,4] must fail cleanly, no OOB write,
     * for both a variable-header-only encoder and one with a payload. */
    {
        uint8_t b[4] = { 0 };
        int cap;

        for (cap = 0; cap <= 4; cap++) {
            TEST_CHECK(mqtt_encode_puback(b, (size_t)cap, 1) ==
                       -MQTT_ERR_BUFSIZE);
            TEST_CHECK(mqtt_encode_publish(b, (size_t)cap, MQTT_STR("t"),
                                            (const uint8_t *)"hi", 2, 0, 0, 0,
                                            0) == -MQTT_ERR_BUFSIZE);
        }
    }
}

static void test_encode_protocol_errors(void)
{
    uint8_t buf[64];

    /* QoS 2 is out of scope (docs/PROTOCOL.md); reject it, don't silently
     * clamp - a caller asking for it has a bug worth surfacing. */
    TEST_CHECK(mqtt_encode_publish(buf, sizeof(buf), MQTT_STR("t"), NULL, 0,
                                    2, 0, 0, 0) == -MQTT_ERR_PROTOCOL);

    {
        mqtt_str topics[1] = { MQTT_STR("a/b") };
        uint8_t qos[1] = { 2 };
        TEST_CHECK(mqtt_encode_subscribe(buf, sizeof(buf), 1, topics, qos, 1) ==
                   -MQTT_ERR_PROTOCOL);
    }
    TEST_CHECK(mqtt_encode_subscribe(buf, sizeof(buf), 1, NULL, NULL, 0) ==
               -MQTT_ERR_PROTOCOL);
}

static void test_decode_vectors(void)
{
    mqtt_packet pkt;
    int n;

    n = mqtt_decode(V_CONNACK_OK, sizeof(V_CONNACK_OK), &pkt);
    TEST_CHECK(n == (int)sizeof(V_CONNACK_OK));
    TEST_CHECK(pkt.type == MQTT_CONNACK);
    TEST_CHECK(pkt.u.connack.session_present == 0);
    TEST_CHECK(pkt.u.connack.return_code == 0);

    n = mqtt_decode(V_CONNACK_REFUSED, sizeof(V_CONNACK_REFUSED), &pkt);
    TEST_CHECK(n == (int)sizeof(V_CONNACK_REFUSED));
    TEST_CHECK(pkt.u.connack.return_code == 5);

    n = mqtt_decode(V_PUBLISH_QOS0, sizeof(V_PUBLISH_QOS0), &pkt);
    TEST_CHECK(n == (int)sizeof(V_PUBLISH_QOS0));
    TEST_CHECK(pkt.type == MQTT_PUBLISH);
    TEST_CHECK(pkt.u.publish.qos == 0);
    TEST_CHECK(pkt.u.publish.retain == 0 && pkt.u.publish.dup == 0);
    TEST_CHECK(pkt.u.publish.topic.len == 1 && pkt.u.publish.topic.ptr[0] == 't');
    TEST_CHECK(pkt.u.publish.payload_len == 2 &&
               memcmp(pkt.u.publish.payload, "hi", 2) == 0);

    n = mqtt_decode(V_PUBLISH_QOS1, sizeof(V_PUBLISH_QOS1), &pkt);
    TEST_CHECK(n == (int)sizeof(V_PUBLISH_QOS1));
    TEST_CHECK(pkt.u.publish.qos == 1);
    TEST_CHECK(pkt.u.publish.retain == 1);
    TEST_CHECK(pkt.u.publish.packet_id == 7);
    TEST_CHECK(pkt.u.publish.payload_len == 2 &&
               memcmp(pkt.u.publish.payload, "hi", 2) == 0);

    n = mqtt_decode(V_PUBACK, sizeof(V_PUBACK), &pkt);
    TEST_CHECK(n == (int)sizeof(V_PUBACK) && pkt.type == MQTT_PUBACK &&
               pkt.u.ack.packet_id == 7);

    n = mqtt_decode(V_PUBREC, sizeof(V_PUBREC), &pkt);
    TEST_CHECK(n == (int)sizeof(V_PUBREC) && pkt.type == MQTT_PUBREC &&
               pkt.u.ack.packet_id == 7);

    n = mqtt_decode(V_PUBREL, sizeof(V_PUBREL), &pkt);
    TEST_CHECK(n == (int)sizeof(V_PUBREL) && pkt.type == MQTT_PUBREL &&
               pkt.u.ack.packet_id == 7);

    n = mqtt_decode(V_PUBCOMP, sizeof(V_PUBCOMP), &pkt);
    TEST_CHECK(n == (int)sizeof(V_PUBCOMP) && pkt.type == MQTT_PUBCOMP &&
               pkt.u.ack.packet_id == 7);

    n = mqtt_decode(V_SUBACK, sizeof(V_SUBACK), &pkt);
    TEST_CHECK(n == (int)sizeof(V_SUBACK) && pkt.type == MQTT_SUBACK);
    TEST_CHECK(pkt.u.suback.packet_id == 10);
    TEST_CHECK(pkt.u.suback.count == 1 && pkt.u.suback.codes[0] == 0x01);

    n = mqtt_decode(V_UNSUBACK, sizeof(V_UNSUBACK), &pkt);
    TEST_CHECK(n == (int)sizeof(V_UNSUBACK) && pkt.type == MQTT_UNSUBACK &&
               pkt.u.ack.packet_id == 11);

    n = mqtt_decode(V_PINGRESP, sizeof(V_PINGRESP), &pkt);
    TEST_CHECK(n == (int)sizeof(V_PINGRESP) && pkt.type == MQTT_PINGRESP);
}

/* CONNECT/SUBSCRIBE/UNSUBSCRIBE/PINGREQ/DISCONNECT are client->broker only;
 * mqtt_decode() must not silently accept them (see mqtt_packet.h). */
static void test_decode_rejects_client_to_broker_types(void)
{
    mqtt_packet pkt;

    TEST_CHECK(mqtt_decode(V_CONNECT, sizeof(V_CONNECT), &pkt) ==
               -MQTT_ERR_MALFORMED);
    TEST_CHECK(mqtt_decode(V_SUBSCRIBE, sizeof(V_SUBSCRIBE), &pkt) ==
               -MQTT_ERR_MALFORMED);
    TEST_CHECK(mqtt_decode(V_UNSUBSCRIBE, sizeof(V_UNSUBSCRIBE), &pkt) ==
               -MQTT_ERR_MALFORMED);
    TEST_CHECK(mqtt_decode(V_PINGREQ, sizeof(V_PINGREQ), &pkt) ==
               -MQTT_ERR_MALFORMED);
    TEST_CHECK(mqtt_decode(V_DISCONNECT, sizeof(V_DISCONNECT), &pkt) ==
               -MQTT_ERR_MALFORMED);
}

static void test_decode_malformed_flags(void)
{
    mqtt_packet pkt;
    uint8_t bad[sizeof(V_CONNACK_OK)];

    /* CONNACK fixed-header flags must be 0. */
    memcpy(bad, V_CONNACK_OK, sizeof(bad));
    bad[0] = 0x21; /* type CONNACK, flags 0x1 */
    TEST_CHECK(mqtt_decode(bad, sizeof(bad), &pkt) == -MQTT_ERR_MALFORMED);

    /* CONNACK byte 2 has only bit 0 defined. */
    memcpy(bad, V_CONNACK_OK, sizeof(bad));
    bad[2] = 0x02;
    TEST_CHECK(mqtt_decode(bad, sizeof(bad), &pkt) == -MQTT_ERR_MALFORMED);

    /* PUBREL must carry reserved flags 0010; wrong flags reject. */
    {
        uint8_t badrel[sizeof(V_PUBREL)];
        memcpy(badrel, V_PUBREL, sizeof(badrel));
        badrel[0] = (uint8_t)(MQTT_PUBREL << 4); /* flags 0x0 instead of 0x2 */
        TEST_CHECK(mqtt_decode(badrel, sizeof(badrel), &pkt) ==
                   -MQTT_ERR_MALFORMED);
    }
}

/* MQTT-4.7.3-1: PUBLISH topic name must not be empty. */
static void test_decode_publish_rejects_empty_topic(void)
{
    mqtt_packet pkt;
    uint8_t bad[sizeof(V_PUBLISH_QOS0)];

    memcpy(bad, V_PUBLISH_QOS0, sizeof(bad));
    /* V_PUBLISH_QOS0 topic length is bytes [2:4) big-endian; zero it and
     * drop the now-absent topic byte from the remaining length (byte 1). */
    bad[1] = (uint8_t)(bad[1] - 1);
    bad[2] = 0;
    bad[3] = 0;
    TEST_CHECK(mqtt_decode(bad, sizeof(bad) - 1, &pkt) == -MQTT_ERR_MALFORMED);
}

static void test_truncation_sweeps(void)
{
    check_truncation_sweep(V_CONNACK_OK, sizeof(V_CONNACK_OK));
    check_truncation_sweep(V_PUBLISH_QOS0, sizeof(V_PUBLISH_QOS0));
    check_truncation_sweep(V_PUBLISH_QOS1, sizeof(V_PUBLISH_QOS1));
    check_truncation_sweep(V_PUBACK, sizeof(V_PUBACK));
    check_truncation_sweep(V_PUBREL, sizeof(V_PUBREL));
    check_truncation_sweep(V_SUBACK, sizeof(V_SUBACK));
    check_truncation_sweep(V_UNSUBACK, sizeof(V_UNSUBACK));
    check_truncation_sweep(V_PINGRESP, sizeof(V_PINGRESP));
}

void run_codec_tests(void)
{
    test_remlen();
    test_encode_vectors();
    test_encode_bufsize();
    test_encode_protocol_errors();
    test_decode_vectors();
    test_decode_rejects_client_to_broker_types();
    test_decode_malformed_flags();
    test_decode_publish_rejects_empty_topic();
    test_truncation_sweeps();
}
