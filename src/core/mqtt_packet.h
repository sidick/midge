#ifndef MIDGE_MQTT_PACKET_H
#define MIDGE_MQTT_PACKET_H

/* MQTT 3.1.1 packet codec. Portable C99, no OS dependencies, no allocation -
 * callers supply all storage. This is the layer that will eventually become
 * the body of mqtt.library (see docs/ARCHITECTURE.md), so it must stay free
 * of platform assumptions and of any state that couldn't be duplicated per
 * connection. */

#include <stddef.h>
#include <stdint.h>

/* MQTT 3.1.1 control packet types (fixed header type nibble). */
#define MQTT_CONNECT     1
#define MQTT_CONNACK     2
#define MQTT_PUBLISH     3
#define MQTT_PUBACK      4
#define MQTT_PUBREC      5
#define MQTT_PUBREL      6
#define MQTT_PUBCOMP     7
#define MQTT_SUBSCRIBE   8
#define MQTT_SUBACK      9
#define MQTT_UNSUBSCRIBE 10
#define MQTT_UNSUBACK    11
#define MQTT_PINGREQ     12
#define MQTT_PINGRESP    13
#define MQTT_DISCONNECT  14

typedef enum {
    MQTT_OK = 0,
    MQTT_ERR_INCOMPLETE, /* not a full packet yet; caller should read more */
    MQTT_ERR_MALFORMED,  /* bytes present but violate the wire format */
    MQTT_ERR_BUFSIZE,    /* output buffer too small */
    MQTT_ERR_PROTOCOL    /* an argument is out of the protocol's legal range */
} mqtt_err;

/* Non-owning view into caller-supplied storage. On encode input, ptr/len
 * describe the caller's string. On decode output, ptr aliases the decoded
 * buffer - valid only as long as that buffer is, and only until the next
 * mqtt_decode() call on it. */
typedef struct {
    const char *ptr;
    uint16_t len;
} mqtt_str;

#define MQTT_STR(lit) ((mqtt_str){ (lit), (uint16_t)(sizeof(lit) - 1) })
#define MQTT_STR_NULL ((mqtt_str){ NULL, 0 })

/* --- Remaining-length varint (MQTT 3.1.1 section 2.2.3) --- */

/* Encodes `value` (0..268435455) into up to 4 bytes at `out`, which must
 * have room for 4 bytes regardless of the eventual length. Returns the
 * number of bytes written (1-4), or a negative mqtt_err. */
int mqtt_remlen_encode(uint8_t *out, uint32_t value);

/* Decodes a remaining-length varint from the first `avail` bytes of `in`.
 * Returns bytes consumed (1-4) on success, 0 if `in` does not yet contain a
 * complete varint (caller should read more), or a negative mqtt_err if the
 * bytes present are malformed (continuation bit still set after 4 bytes). */
int mqtt_remlen_decode(const uint8_t *in, size_t avail, uint32_t *value);

/* --- Encoders ---
 * Each writes one complete packet (fixed header + variable header + payload)
 * into `buf` and returns the total length written, or a negative mqtt_err
 * (MQTT_ERR_BUFSIZE if too small, MQTT_ERR_PROTOCOL if an argument is out of
 * range). No allocation - `buf` is the only scratch space used.
 *
 * `buf` must have at least 5 bytes more capacity than the packet's final
 * size: the variable header/payload is built starting at buf+5 (room for
 * the largest possible 1-byte-type + 4-byte-remaining-length fixed header)
 * and then compacted down once the real length is known, so `cap` is
 * checked against that 5-byte-inflated size, not the final one. */

typedef struct {
    mqtt_str client_id;
    mqtt_str username;   /* MQTT_STR_NULL if absent */
    mqtt_str password;   /* MQTT_STR_NULL if absent; ignored without username */
    mqtt_str will_topic;  /* MQTT_STR_NULL if there is no will */
    mqtt_str will_message;
    uint8_t will_qos;     /* 0 or 1 - QoS 2 is out of scope, see docs/PROTOCOL.md */
    int will_retain;
    int clean_session;
    uint16_t keepalive;  /* seconds */
} mqtt_connect_opts;

int mqtt_encode_connect(uint8_t *buf, size_t cap, const mqtt_connect_opts *o);

int mqtt_encode_publish(uint8_t *buf, size_t cap, mqtt_str topic,
                         const uint8_t *payload, size_t payload_len,
                         uint8_t qos, int retain, int dup, uint16_t packet_id);

int mqtt_encode_puback(uint8_t *buf, size_t cap, uint16_t packet_id);
int mqtt_encode_pubrec(uint8_t *buf, size_t cap, uint16_t packet_id);
int mqtt_encode_pubrel(uint8_t *buf, size_t cap, uint16_t packet_id);
int mqtt_encode_pubcomp(uint8_t *buf, size_t cap, uint16_t packet_id);

/* `topics` and `qos` are parallel arrays of length `count`. */
int mqtt_encode_subscribe(uint8_t *buf, size_t cap, uint16_t packet_id,
                           const mqtt_str *topics, const uint8_t *qos,
                           int count);
int mqtt_encode_unsubscribe(uint8_t *buf, size_t cap, uint16_t packet_id,
                             const mqtt_str *topics, int count);

int mqtt_encode_pingreq(uint8_t *buf, size_t cap);
int mqtt_encode_disconnect(uint8_t *buf, size_t cap);

/* --- Decoder ---
 * Decodes the packet types a client receives from a broker: CONNACK,
 * PUBLISH, PUBACK, PUBREC, PUBREL, PUBCOMP, SUBACK, UNSUBACK, PINGRESP.
 * CONNECT, SUBSCRIBE, UNSUBSCRIBE, PINGREQ and DISCONNECT are client->broker
 * only - a client never needs to parse them back, so this decoder rejects
 * them as MQTT_ERR_MALFORMED; their encode-side correctness is instead
 * covered by byte-exact vectors in tests/test_codec.c. */
typedef struct {
    uint8_t type;
    uint8_t flags;
    union {
        struct {
            uint8_t session_present;
            uint8_t return_code;
        } connack;
        struct {
            mqtt_str topic;
            const uint8_t *payload;
            uint32_t payload_len;
            uint8_t qos;
            uint8_t retain;
            uint8_t dup;
            uint16_t packet_id; /* only meaningful if qos > 0 */
        } publish;
        struct {
            uint16_t packet_id;
        } ack; /* PUBACK / PUBREC / PUBREL / PUBCOMP / UNSUBACK */
        struct {
            uint16_t packet_id;
            const uint8_t *codes; /* one byte per subscribed topic filter */
            int count;
        } suback;
    } u;
} mqtt_packet;

/* Attempts to decode one packet from the first `avail` bytes of `buf`.
 * Returns bytes consumed (> 0) on success, 0 if `buf` does not yet hold a
 * complete packet, or a negative mqtt_err on a malformed packet. */
int mqtt_decode(const uint8_t *buf, size_t avail, mqtt_packet *out);

#endif
