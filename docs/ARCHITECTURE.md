# Architecture

midge implements the proposal at `project-ideas/pending/amimqtt-proposal.md`
(the original rationale for the project): a native MQTT 3.1.1 client for
classic AmigaOS, delivered as `mqtt.library` plus a ClassAct/ReAction
dashboard. This document covers how the code is laid out and where it's
headed; `docs/PROTOCOL.md` covers MQTT-specific implementation notes.

## Layering

```
src/core/    portable C99, zero OS/libc-beyond-C99 dependencies
             mqtt_packet.[ch]    packet codec (encode/decode)
             mqtt_client.[ch]    connection state machine
             mqtt_transport.h    the transport vtable (the only seam below)

src/host/    host-native platform glue (BSD sockets, getopt)
src/amiga/   AmigaOS platform glue (bsdsocket.library, ReadArgs)
src/tools/   mqtt_pub/mqtt_sub logic shared by both front-ends
```

`src/core` never touches a socket, a library base, a clock, or an
allocator - every packet function takes caller-supplied buffers, and
`mqtt_client` is driven by a caller-supplied clock (`now_ms`) and a
transport (`mqtt_transport`, below). That's what lets it unit-test on the
host against a fake transport and fake clock (`tests/test_client.c`), and
it's a hard requirement going forward: this is the future body of
`mqtt.library` (Phase 2), which must support multiple simultaneous
connections with no shared state between them.

`src/tools` holds `mqtt_pub_run`/`mqtt_sub_run` - the actual pub/sub logic,
parameterized over a `tool_opts` struct and an already-connected
`mqtt_transport`. `src/host` and `src/amiga` each contribute a transport
implementation and an argument parser (getopt vs. ReadArgs) and call into
that shared logic; this is the only code the two CLI tools share beyond
`src/core`.

## The transport vtable

```c
typedef struct mqtt_transport {
    void *ctx;
    int  (*send)(void *ctx, const uint8_t *buf, size_t len);
    int  (*recv)(void *ctx, uint8_t *buf, size_t cap);
    void (*close)(void *ctx);
} mqtt_transport;
```

This is the one seam between the portable core and everything
platform-specific. Today it has two implementations:
`src/host/transport_bsd.c` (BSD sockets, blocking with a short receive
timeout) and `src/amiga/transport_bsdsocket.c` (bsdsocket.library, same
polling-timeout shape via `WaitSelect()` with `SIGBREAKF_CTRL_C` in the
mask). Phase 3 (TLS) adds AmiSSL and OpenSSL implementations behind the
same vtable - `mqtt_client` doesn't change at all.

## Roadmap

- **Phase 1 (this release).** Protocol core + `mqtt_pub`/`mqtt_sub` CLI
  tools. QoS 0 publish, QoS 0/1 subscribe (an incoming QoS 1 PUBLISH is
  auto-acknowledged), keepalive scheduling and timeout detection. See
  `docs/PROTOCOL.md` for exact coverage.
- **Phase 2 - mqtt.library.** Wraps `src/core` as a shared AmigaOS library:
  `.fd` file + LVO table, a `Resident`/romtag, `libinit`, per-object
  compilation, and base-register discipline (baserel or an explicit
  library base) - none of which exists anywhere in this codebase yet, so
  this is the project's main remaining technical risk. Also adds:
  subprocess-per-connection owning the socket (bsdsocket handles are
  per-task - see `CLAUDE.md`), MsgPort dispatch of incoming PUBLISHes to
  the caller's own event loop, QoS 1 with retransmission, reconnect/backoff
  with auto-resubscribe, autodocs, and two example programs. The CLI tools
  relink against the library once it exists.
- **Phase 3 - TLS.** AmiSSL 5.x as a third `mqtt_transport` implementation
  behind an `MQTT_TLS` tag; soft-fails when `amissl.library` is absent. An
  OpenSSL host transport gives test parity.
- **Phase 4 - Dashboard.** A ClassAct/ReAction application: a widget grid
  (label/gauge/switch/button) via `layout.gadget`, a plain-text config file,
  minimal JSON field extraction for Home Assistant-style payloads, and HA
  MQTT discovery publishing the Amiga's own telemetry (uptime, chip/fast RAM
  free, CPU model).
- **Phase 5 - Release.** Aminet upload, a Home Assistant walkthrough in
  `userdocs/`, an installer, demo capture.

## Testing strategy

| Tier | What it proves |
|---|---|
| `make test` | Codec vectors and the client state machine, against a fake transport and fake clock - no OS, no network. |
| `make broker-smoke` | The host CLI tools against a real Mosquitto - real MQTT wire protocol, real TCP, just not on m68k. |
| `make test-target` | The codec compiled for a real big-endian 68020, under Copperline, with no networking involved. |
| `make net-smoke` | The Amiga `bsdsocket.library` transport end to end: cross-built `mqtt_pub` under Copperline reaching a real Mosquitto on the host via the HostSocket board - see `tests/net/README.md`. |

`make net-smoke`'s design is worth calling out: an earlier sketch assumed a
real guest TCP/IP stack (Roadshow/AmiTCP) on a user-supplied Workbench
image, which would have made the test local-only and gated behind
machine-specific assets. Copperline's bundled HostSocket board
(`[hostsocket] net = "host"`) removes that need entirely - the guest's
`bsdsocket.library` autoboots from the board's own ROM code, delegating
socket calls straight to the host - so this test needs nothing beyond
`copperline` and `mosquitto` on `PATH` and runs in CI as well as locally.
