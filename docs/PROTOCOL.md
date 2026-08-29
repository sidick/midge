# MQTT 3.1.1 implementation notes

midge implements OASIS MQTT version 3.1.1. This document tracks what's
covered, what's deliberately deferred, and why - `docs/ARCHITECTURE.md`
covers the code layout this lives in.

## Packet coverage

`src/core/mqtt_packet.c` encodes all 14 packet types. `mqtt_decode()`
only decodes the types a client actually receives from a broker:

| Type | Encode | Decode |
|---|---|---|
| CONNECT | yes | n/a (client -> broker only) |
| CONNACK | n/a | yes |
| PUBLISH | yes | yes |
| PUBACK | yes | yes |
| PUBREC | yes | yes |
| PUBREL | yes | yes |
| PUBCOMP | yes | yes |
| SUBSCRIBE | yes | n/a (client -> broker only) |
| SUBACK | n/a | yes |
| UNSUBSCRIBE | yes | n/a (client -> broker only) |
| UNSUBACK | n/a | yes |
| PINGREQ | yes | n/a (client -> broker only) |
| PINGRESP | n/a | yes |
| DISCONNECT | yes | n/a (client -> broker only) |

A client never needs to parse the packet types it only ever sends, so
`mqtt_decode()` rejects them as malformed rather than carrying unused
parsing paths; their encode-side correctness is instead covered by
byte-exact vectors (`tests/vectors.h`, built by hand from the spec) in
`tests/test_codec.c`.

## QoS scope

- **QoS 0** is fully supported both directions.
- **QoS 1** subscribe/receive is supported: `mqtt_client_process()`
  auto-acknowledges an incoming QoS 1 PUBLISH with a PUBACK.
  **QoS 1 publish (outbound)** is not yet exposed - `mqtt_client_publish()`
  only sends QoS 0, and `mqtt_pub`/`mqtt_sub` reject `-q 1`/`QOS 1` on the
  publish side with a message pointing at Phase 2. Outbound QoS 1 needs
  retransmission-on-timeout and (for a persistent session) dedup state,
  which belongs with the reconnect/session work `mqtt.library` brings in
  Phase 2, not bolted onto the CLI tools first.
- **QoS 2 is out of scope entirely**, in this release and for the
  foreseeable roadmap. It roughly doubles the state machine (a 4-packet
  handshake with its own dedup/retry rules) for a guarantee
  (exactly-once) that essentially no smart-home broker/device pairing
  actually needs - QoS 1 (at-least-once, deduplicated at the
  application level if it matters) covers every realistic use case this
  project targets. `mqtt_encode_publish()`, `mqtt_encode_subscribe()`, and
  the CONNECT will builder all reject `qos == 2` with `MQTT_ERR_PROTOCOL`
  rather than silently downgrading it.

## Keepalive

`mqtt_client_process()` schedules a PINGREQ once 3/4 of the keepalive
interval has elapsed since the client last sent anything, and treats a
missing PINGRESP after a full keepalive interval as a fatal connection
error (`MQTT_CLIENT_ERR_KEEPALIVE_TIMEOUT`) - the same 3/4 convention
common MQTT client libraries use, giving one full round-trip of slack
before the broker's own keepalive×1.5 grace period (spec section 3.1.2.10)
would drop the connection first. A CONNECT that never gets a CONNACK within
one keepalive period is treated the same way
(`MQTT_CLIENT_ERR_CONNECT_TIMEOUT`).

One caveat, documented in `src/core/mqtt_client.h`: `mqtt_client_publish()`
and `mqtt_client_subscribe()` don't take a clock argument (matching the
approved implementation plan), so they don't reset the "last sent"
timer - only `mqtt_client_connect()` and `mqtt_client_process()`'s own
PINGREQ do. In practice this means a PINGREQ may go out slightly earlier
than the theoretical minimum, never later, so it doesn't threaten the
timeout guarantee above.

## Session state

`mqtt_client_connect()` always sets Clean Session (bit 1 of the CONNECT
flags). Persistent sessions (broker-retained subscriptions and queued QoS 1
messages across reconnects) are part of the Phase 2 reconnect/resubscribe
work, not this release.
