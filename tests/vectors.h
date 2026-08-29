/* vectors.h — byte-exact MQTT 3.1.1 packet vectors, hand-built from the
 * spec (OASIS mqtt-v3.1.1, section 3). Shared between the host codec tests
 * (tests/test_codec.c) and the on-target Copperline self-test
 * (tests/copperline/codec_selftest.c) so both exercise identical data. */
#ifndef MIDGE_VECTORS_H
#define MIDGE_VECTORS_H

#include <stdint.h>

/* CONNECT: client_id="mid1", clean_session=1, keepalive=60, no will/user/pass. */
static const uint8_t V_CONNECT[] = {
    0x10, 0x10,
    0x00, 0x04, 'M', 'Q', 'T', 'T',
    0x04,
    0x02,
    0x00, 0x3C,
    0x00, 0x04, 'm', 'i', 'd', '1'
};

/* CONNACK: session not present, return code 0 (accepted). */
static const uint8_t V_CONNACK_OK[] = { 0x20, 0x02, 0x00, 0x00 };

/* CONNACK: return code 5 (not authorized). */
static const uint8_t V_CONNACK_REFUSED[] = { 0x20, 0x02, 0x00, 0x05 };

/* PUBLISH: topic="t", payload="hi", QoS 0, no retain, no dup. */
static const uint8_t V_PUBLISH_QOS0[] = {
    0x30, 0x05,
    0x00, 0x01, 't',
    'h', 'i'
};

/* PUBLISH: topic="t", payload="hi", QoS 1, packet_id=7, retain=1, dup=0. */
static const uint8_t V_PUBLISH_QOS1[] = {
    0x33, 0x07,
    0x00, 0x01, 't',
    0x00, 0x07,
    'h', 'i'
};

/* PUBACK / PUBREC / PUBCOMP: packet_id=7. */
static const uint8_t V_PUBACK[]  = { 0x40, 0x02, 0x00, 0x07 };
static const uint8_t V_PUBREC[]  = { 0x50, 0x02, 0x00, 0x07 };
static const uint8_t V_PUBCOMP[] = { 0x70, 0x02, 0x00, 0x07 };

/* PUBREL: packet_id=7 (reserved fixed-header bits 0010). */
static const uint8_t V_PUBREL[] = { 0x62, 0x02, 0x00, 0x07 };

/* SUBSCRIBE: packet_id=10, one filter "a/b" requesting QoS 1. */
static const uint8_t V_SUBSCRIBE[] = {
    0x82, 0x08,
    0x00, 0x0A,
    0x00, 0x03, 'a', '/', 'b',
    0x01
};

/* SUBACK: packet_id=10, one return code (granted QoS 1). */
static const uint8_t V_SUBACK[] = { 0x90, 0x03, 0x00, 0x0A, 0x01 };

/* UNSUBSCRIBE: packet_id=11, one filter "a/b". */
static const uint8_t V_UNSUBSCRIBE[] = {
    0xA2, 0x07,
    0x00, 0x0B,
    0x00, 0x03, 'a', '/', 'b'
};

/* UNSUBACK: packet_id=11. */
static const uint8_t V_UNSUBACK[] = { 0xB0, 0x02, 0x00, 0x0B };

static const uint8_t V_PINGREQ[]    = { 0xC0, 0x00 };
static const uint8_t V_PINGRESP[]   = { 0xD0, 0x00 };
static const uint8_t V_DISCONNECT[] = { 0xE0, 0x00 };

#endif /* MIDGE_VECTORS_H */
