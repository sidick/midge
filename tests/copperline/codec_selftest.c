/* codec_selftest.c — on-target (m68k/AmigaOS) smoke test for the MQTT
 * packet codec (src/core/mqtt_packet.c). Runs a representative subset of
 * the same vectors as tests/test_codec.c (shared via tests/vectors.h) and
 * emits PASS/FAIL lines over the serial port via exec/RawPutChar - the ROM
 * debug path, which needs no serial.device handler, no Mount, and no
 * Workbench files. Copperline's `--serial stdout` forwards it to the host;
 * run.sh checks the result.
 *
 * This is the on-target risk the host tests can't cover: codec correctness
 * (struct layout, bitfield/pointer arithmetic, integer promotion) as
 * actually compiled for a big-endian 68020. No networking is involved here
 * - see tests/net/ for that, and tests/test_codec.c for the exhaustive
 * host-side vector/truncation/malformed-input coverage this doesn't repeat. */

#include "mqtt_packet.h"
#include "vectors.h"

/* exec RawPutChar: char in d0, SysBase (absolute 4) in a6, LVO -516. */
static void raw_put(char c)
{
    void *SysBase = *(void **)4UL;
    register long d0 __asm__("d0") = (unsigned char)c;
    register void *a6 __asm__("a6") = SysBase;
    __asm__ volatile("jsr -516(%%a6)" : : "r"(d0), "r"(a6)
                     : "d1", "a0", "a1", "cc", "memory");
}

static void raw_str(const char *s)
{
    while (*s)
        raw_put(*s++);
}

static int g_fails;

static void check(int cond, const char *name)
{
    if (cond) {
        raw_str("PASS ");
    } else {
        raw_str("FAIL ");
        g_fails++;
    }
    raw_str(name);
    raw_str("\r\n");
}

static int bytes_eq(const uint8_t *a, int alen, const uint8_t *b, int blen)
{
    int i;
    if (alen != blen)
        return 0;
    for (i = 0; i < alen; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int main(void)
{
    uint8_t buf[64];
    uint32_t v;
    int n;

    raw_str("BEGIN\r\n");

    /* Remaining-length varint boundaries. */
    n = mqtt_remlen_encode(buf, 127);
    check(n == 1 && buf[0] == 0x7F, "remlen-127");
    n = mqtt_remlen_encode(buf, 16384);
    check(n == 3 && buf[0] == 0x80 && buf[1] == 0x80 && buf[2] == 0x01,
          "remlen-16384");
    n = mqtt_remlen_encode(buf, 268435455);
    check(mqtt_remlen_decode(buf, (size_t)n, &v) == n && v == 268435455u,
          "remlen-268435455-roundtrip");

    /* Encoders against byte-exact vectors. */
    {
        mqtt_connect_opts co;
        co.client_id = MQTT_STR("mid1");
        co.username = MQTT_STR_NULL;
        co.password = MQTT_STR_NULL;
        co.will_topic = MQTT_STR_NULL;
        co.will_message = MQTT_STR_NULL;
        co.will_qos = 0;
        co.will_retain = 0;
        co.clean_session = 1;
        co.keepalive = 60;
        n = mqtt_encode_connect(buf, sizeof(buf), &co);
        check(bytes_eq(buf, n, V_CONNECT, (int)sizeof(V_CONNECT)), "encode-connect");
    }

    n = mqtt_encode_publish(buf, sizeof(buf), MQTT_STR("t"),
                             (const uint8_t *)"hi", 2, 0, 0, 0, 0);
    check(bytes_eq(buf, n, V_PUBLISH_QOS0, (int)sizeof(V_PUBLISH_QOS0)),
          "encode-publish-qos0");

    n = mqtt_encode_publish(buf, sizeof(buf), MQTT_STR("t"),
                             (const uint8_t *)"hi", 2, 1, 1, 0, 7);
    check(bytes_eq(buf, n, V_PUBLISH_QOS1, (int)sizeof(V_PUBLISH_QOS1)),
          "encode-publish-qos1");

    {
        mqtt_str topics[1] = { MQTT_STR("a/b") };
        uint8_t qos[1] = { 1 };
        n = mqtt_encode_subscribe(buf, sizeof(buf), 10, topics, qos, 1);
        check(bytes_eq(buf, n, V_SUBSCRIBE, (int)sizeof(V_SUBSCRIBE)),
              "encode-subscribe");
    }

    /* Decoders against byte-exact vectors. */
    {
        mqtt_packet pkt;
        n = mqtt_decode(V_CONNACK_OK, sizeof(V_CONNACK_OK), &pkt);
        check(n == (int)sizeof(V_CONNACK_OK) && pkt.type == MQTT_CONNACK &&
                  pkt.u.connack.return_code == 0,
              "decode-connack");
    }

    {
        mqtt_packet pkt;
        n = mqtt_decode(V_PUBLISH_QOS1, sizeof(V_PUBLISH_QOS1), &pkt);
        check(n == (int)sizeof(V_PUBLISH_QOS1) && pkt.u.publish.qos == 1 &&
                  pkt.u.publish.retain == 1 && pkt.u.publish.packet_id == 7 &&
                  pkt.u.publish.topic.len == 1 &&
                  pkt.u.publish.payload_len == 2 &&
                  pkt.u.publish.payload[0] == 'h' && pkt.u.publish.payload[1] == 'i',
              "decode-publish-qos1");
    }

    {
        mqtt_packet pkt;
        n = mqtt_decode(V_SUBACK, sizeof(V_SUBACK), &pkt);
        check(n == (int)sizeof(V_SUBACK) && pkt.u.suback.packet_id == 10 &&
                  pkt.u.suback.count == 1 && pkt.u.suback.codes[0] == 0x01,
              "decode-suback");
    }

    /* Truncation: every proper prefix must report incomplete (0), never a
     * false decode or an out-of-bounds read. */
    {
        mqtt_packet pkt;
        int ok = 1;
        size_t i;
        for (i = 0; i < sizeof(V_PUBACK); i++)
            if (mqtt_decode(V_PUBACK, i, &pkt) != 0)
                ok = 0;
        check(ok, "truncation-sweep-puback");
    }

    raw_str(g_fails == 0 ? "RESULT=OK\r\n" : "RESULT=FAIL\r\n");
    raw_str("END\r\n");
    return g_fails;
}
