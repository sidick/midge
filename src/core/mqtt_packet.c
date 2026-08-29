#include "mqtt_packet.h"

#include <string.h>

#define MQTT_REMLEN_MAX 268435455UL /* 0x0FFFFFFF: largest 4-byte varint */

int mqtt_remlen_encode(uint8_t *out, uint32_t value)
{
    int i = 0;

    if (value > MQTT_REMLEN_MAX)
        return -MQTT_ERR_PROTOCOL;

    do {
        uint8_t b = (uint8_t)(value % 128);
        value /= 128;
        if (value > 0)
            b |= 0x80;
        out[i++] = b;
    } while (value > 0);

    return i;
}

int mqtt_remlen_decode(const uint8_t *in, size_t avail, uint32_t *value)
{
    uint32_t v = 0, mult = 1;
    size_t i;

    for (i = 0; i < 4; i++) {
        uint8_t b;
        if (i >= avail)
            return 0; /* incomplete */
        b = in[i];
        v += (uint32_t)(b & 0x7F) * mult;
        if (!(b & 0x80)) {
            *value = v;
            return (int)(i + 1);
        }
        mult *= 128;
    }
    return -MQTT_ERR_MALFORMED; /* continuation bit set after 4 bytes */
}

/* --- Bounds-checked cursor writer, used by every encoder below. Content is
 * always built starting at offset 5 (see the header comment on `buf`'s
 * required headroom); finish_packet() compacts it down once the real
 * remaining-length is known. */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    int err;
} wbuf;

static void w_init(wbuf *w, uint8_t *buf, size_t cap, size_t start)
{
    w->buf = buf;
    w->cap = cap;
    w->pos = start;
    w->err = 0;
}

static void w_bytes(wbuf *w, const void *p, size_t n)
{
    if (w->err)
        return;
    if (n > w->cap - w->pos) { /* avoids pos+n overflow */
        w->err = MQTT_ERR_BUFSIZE;
        return;
    }
    if (n > 0)
        memcpy(w->buf + w->pos, p, n);
    w->pos += n;
}

static void w_u8(wbuf *w, uint8_t v)
{
    w_bytes(w, &v, 1);
}

static void w_u16(wbuf *w, uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
    w_bytes(w, b, 2);
}

static void w_str(wbuf *w, mqtt_str s)
{
    w_u16(w, s.len);
    w_bytes(w, s.ptr, s.len);
}

static int finish_packet(uint8_t *buf, size_t cap, uint8_t type_flags,
                          size_t content_len)
{
    uint8_t remlen_buf[4];
    int remlen_bytes = mqtt_remlen_encode(remlen_buf, (uint32_t)content_len);
    size_t header_len;

    if (remlen_bytes < 0)
        return remlen_bytes;
    header_len = 1 + (size_t)remlen_bytes;
    if (header_len + content_len > cap)
        return -MQTT_ERR_BUFSIZE;

    if (content_len > 0)
        memmove(buf + header_len, buf + 5, content_len);
    buf[0] = type_flags;
    memcpy(buf + 1, remlen_buf, (size_t)remlen_bytes);

    return (int)(header_len + content_len);
}

int mqtt_encode_connect(uint8_t *buf, size_t cap, const mqtt_connect_opts *o)
{
    wbuf w;
    uint8_t flags = 0;

    if (o->will_qos > 1)
        return -MQTT_ERR_PROTOCOL; /* QoS 2 out of scope, see docs/PROTOCOL.md */

    w_init(&w, buf, cap, 5);
    w_str(&w, MQTT_STR("MQTT"));
    w_u8(&w, 4); /* protocol level: MQTT 3.1.1 */

    if (o->clean_session)
        flags |= 0x02;
    if (o->will_topic.ptr) {
        flags |= 0x04;
        flags |= (uint8_t)(o->will_qos << 3);
        if (o->will_retain)
            flags |= 0x20;
    }
    if (o->username.ptr) {
        flags |= 0x80;
        if (o->password.ptr)
            flags |= 0x40;
    }
    w_u8(&w, flags);
    w_u16(&w, o->keepalive);

    w_str(&w, o->client_id);
    if (o->will_topic.ptr) {
        w_str(&w, o->will_topic);
        w_str(&w, o->will_message);
    }
    if (o->username.ptr) {
        w_str(&w, o->username);
        if (o->password.ptr)
            w_str(&w, o->password);
    }

    if (w.err)
        return -w.err;
    return finish_packet(buf, cap, (uint8_t)(MQTT_CONNECT << 4), w.pos - 5);
}

int mqtt_encode_publish(uint8_t *buf, size_t cap, mqtt_str topic,
                         const uint8_t *payload, size_t payload_len,
                         uint8_t qos, int retain, int dup, uint16_t packet_id)
{
    wbuf w;
    uint8_t type_flags;

    if (qos > 1)
        return -MQTT_ERR_PROTOCOL; /* QoS 2 out of scope, see docs/PROTOCOL.md */

    w_init(&w, buf, cap, 5);
    w_str(&w, topic);
    if (qos > 0)
        w_u16(&w, packet_id);
    w_bytes(&w, payload, payload_len);
    if (w.err)
        return -w.err;

    type_flags = (uint8_t)(MQTT_PUBLISH << 4);
    if (dup)
        type_flags |= 0x08;
    type_flags |= (uint8_t)(qos << 1);
    if (retain)
        type_flags |= 0x01;

    return finish_packet(buf, cap, type_flags, w.pos - 5);
}

static int encode_ack(uint8_t *buf, size_t cap, uint8_t type, uint8_t flags,
                       uint16_t packet_id)
{
    wbuf w;

    w_init(&w, buf, cap, 5);
    w_u16(&w, packet_id);
    if (w.err)
        return -w.err;
    return finish_packet(buf, cap, (uint8_t)((type << 4) | flags), w.pos - 5);
}

int mqtt_encode_puback(uint8_t *buf, size_t cap, uint16_t packet_id)
{
    return encode_ack(buf, cap, MQTT_PUBACK, 0x00, packet_id);
}

int mqtt_encode_pubrec(uint8_t *buf, size_t cap, uint16_t packet_id)
{
    return encode_ack(buf, cap, MQTT_PUBREC, 0x00, packet_id);
}

int mqtt_encode_pubrel(uint8_t *buf, size_t cap, uint16_t packet_id)
{
    /* Reserved fixed-header bits for PUBREL are 0010 (spec 3.6.1). */
    return encode_ack(buf, cap, MQTT_PUBREL, 0x02, packet_id);
}

int mqtt_encode_pubcomp(uint8_t *buf, size_t cap, uint16_t packet_id)
{
    return encode_ack(buf, cap, MQTT_PUBCOMP, 0x00, packet_id);
}

int mqtt_encode_subscribe(uint8_t *buf, size_t cap, uint16_t packet_id,
                           const mqtt_str *topics, const uint8_t *qos,
                           int count)
{
    wbuf w;
    int i;

    if (count <= 0)
        return -MQTT_ERR_PROTOCOL;

    w_init(&w, buf, cap, 5);
    w_u16(&w, packet_id);
    for (i = 0; i < count; i++) {
        if (qos[i] > 1)
            return -MQTT_ERR_PROTOCOL; /* QoS 2 out of scope */
        w_str(&w, topics[i]);
        w_u8(&w, qos[i]);
    }
    if (w.err)
        return -w.err;

    /* Reserved fixed-header bits for SUBSCRIBE are 0010 (spec 3.8.1). */
    return finish_packet(buf, cap, (uint8_t)((MQTT_SUBSCRIBE << 4) | 0x02),
                          w.pos - 5);
}

int mqtt_encode_unsubscribe(uint8_t *buf, size_t cap, uint16_t packet_id,
                             const mqtt_str *topics, int count)
{
    wbuf w;
    int i;

    if (count <= 0)
        return -MQTT_ERR_PROTOCOL;

    w_init(&w, buf, cap, 5);
    w_u16(&w, packet_id);
    for (i = 0; i < count; i++)
        w_str(&w, topics[i]);
    if (w.err)
        return -w.err;

    /* Reserved fixed-header bits for UNSUBSCRIBE are 0010 (spec 3.10.1). */
    return finish_packet(buf, cap, (uint8_t)((MQTT_UNSUBSCRIBE << 4) | 0x02),
                          w.pos - 5);
}

int mqtt_encode_pingreq(uint8_t *buf, size_t cap)
{
    return finish_packet(buf, cap, (uint8_t)(MQTT_PINGREQ << 4), 0);
}

int mqtt_encode_disconnect(uint8_t *buf, size_t cap)
{
    return finish_packet(buf, cap, (uint8_t)(MQTT_DISCONNECT << 4), 0);
}

int mqtt_decode(const uint8_t *buf, size_t avail, mqtt_packet *out)
{
    uint8_t type, flags;
    uint32_t remlen;
    int rc;
    size_t header_len, total;
    const uint8_t *content;

    if (avail < 1)
        return 0;
    type = (uint8_t)(buf[0] >> 4);
    flags = (uint8_t)(buf[0] & 0x0F);

    rc = mqtt_remlen_decode(buf + 1, avail - 1, &remlen);
    if (rc == 0)
        return 0;
    if (rc < 0)
        return rc;

    header_len = 1 + (size_t)rc;
    total = header_len + remlen;
    if (avail < total)
        return 0;

    content = buf + header_len;
    out->type = type;
    out->flags = flags;

    switch (type) {
    case MQTT_CONNACK:
        if (remlen != 2 || flags != 0x00)
            return -MQTT_ERR_MALFORMED;
        if (content[0] & 0xFE) /* only bit 0 (Session Present) is defined */
            return -MQTT_ERR_MALFORMED;
        out->u.connack.session_present = content[0] & 0x01;
        out->u.connack.return_code = content[1];
        break;

    case MQTT_PUBLISH: {
        uint8_t qos = (uint8_t)((flags >> 1) & 0x03);
        uint16_t tlen;
        size_t pos;

        if (qos == 3)
            return -MQTT_ERR_MALFORMED;
        if (qos > 1)
            return -MQTT_ERR_PROTOCOL; /* QoS 2 out of scope */
        if (remlen < 2)
            return -MQTT_ERR_MALFORMED;
        tlen = (uint16_t)((content[0] << 8) | content[1]);
        pos = 2;
        if (pos + tlen > remlen)
            return -MQTT_ERR_MALFORMED;
        out->u.publish.topic.ptr = (const char *)(content + pos);
        out->u.publish.topic.len = tlen;
        pos += tlen;

        out->u.publish.qos = qos;
        out->u.publish.dup = (uint8_t)((flags >> 3) & 0x01);
        out->u.publish.retain = (uint8_t)(flags & 0x01);
        if (qos > 0) {
            if (pos + 2 > remlen)
                return -MQTT_ERR_MALFORMED;
            out->u.publish.packet_id =
                (uint16_t)((content[pos] << 8) | content[pos + 1]);
            pos += 2;
        } else {
            out->u.publish.packet_id = 0;
        }
        out->u.publish.payload = content + pos;
        out->u.publish.payload_len = (uint32_t)(remlen - pos);
        break;
    }

    case MQTT_PUBACK:
    case MQTT_PUBREC:
    case MQTT_PUBCOMP:
    case MQTT_UNSUBACK:
        if (remlen != 2 || flags != 0x00)
            return -MQTT_ERR_MALFORMED;
        out->u.ack.packet_id = (uint16_t)((content[0] << 8) | content[1]);
        break;

    case MQTT_PUBREL:
        if (remlen != 2 || flags != 0x02)
            return -MQTT_ERR_MALFORMED;
        out->u.ack.packet_id = (uint16_t)((content[0] << 8) | content[1]);
        break;

    case MQTT_SUBACK:
        if (remlen < 3 || flags != 0x00)
            return -MQTT_ERR_MALFORMED;
        out->u.suback.packet_id = (uint16_t)((content[0] << 8) | content[1]);
        out->u.suback.codes = content + 2;
        out->u.suback.count = (int)(remlen - 2);
        break;

    case MQTT_PINGRESP:
        if (remlen != 0 || flags != 0x00)
            return -MQTT_ERR_MALFORMED;
        break;

    default:
        /* CONNECT/SUBSCRIBE/UNSUBSCRIBE/PINGREQ/DISCONNECT: client->broker
         * only, see the header comment on mqtt_decode(). */
        return -MQTT_ERR_MALFORMED;
    }

    return (int)total;
}
