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
src/tools/   mqtt_pub/mqtt_sub logic shared by the static host/Amiga builds
src/library/ mqtt.library (see "mqtt.library" below)
```

`src/core` never touches a socket, a library base, a clock, or an
allocator - every packet function takes caller-supplied buffers, and
`mqtt_client` is driven by a caller-supplied clock (`now_ms`) and a
transport (`mqtt_transport`, below). That's what lets it unit-test on the
host against a fake transport and fake clock (`tests/test_client.c`), and
it's what let `mqtt.library`'s connection subprocess (below) embed it
unchanged, one instance per connection, with no shared state between them.

`src/tools` holds `mqtt_pub_run`/`mqtt_sub_run` - the pub/sub logic behind
the **static** builds (`mqtt_pub-static`/`mqtt_sub-static`), parameterized
over a `tool_opts` struct and an already-connected `mqtt_transport`.
`src/host` and `src/amiga` each contribute a transport implementation and
an argument parser (getopt vs. ReadArgs) and call into that shared logic.
The **default** `mqtt_pub`/`mqtt_sub` binaries are a separate, much
thinner pair of mains (`src/amiga/pub_main_lib.c`/`sub_main_lib.c`) that
reuse `src/amiga/args.c`'s ReadArgs parsing and `tool_opts` for an
identical CLI contract, but call through `mqtt.library` instead of linking
`src/core`/`src/tools` directly - see "Two build flavours" below.

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
mask - plus an extra caller-settable `break_sigmask` so `mqtt.library`'s
connection subprocess can also wake on its command MsgPort, see below).
Phase 3 (TLS) adds AmiSSL and OpenSSL implementations behind the same
vtable - `mqtt_client` doesn't change at all.

## mqtt.library

A real AmigaOS shared library (`build/mqtt.library`), built with bebbo's
amiga-gcc and libnix's `libinit.o` (single shared data segment - no
`-fbaserel`/`geta4()`; nothing library-global is mutable per-connection
state, so there's nothing for a second opener to trample). Its API is
described once in `src/library/mqtt_lib.sfd`, which `sfdc` expands into
everything else: the caller-facing `proto/mqtt.h` + inline-asm macro
header (so C code just does `#include <proto/mqtt.h>` and calls
`MQTT_Connect(...)` directly, no register/LVO knowledge needed), the
library-side gate stub trampolines, the `-1`-terminated function table
`MakeLibrary()` needs, and a classic `.fd` for completeness. The one thing
sfdc doesn't generate - the actual argument/option *types* the API uses
(`struct MqttConnectOpts`, `struct MqttMessage`, the `MQTTERR_*` codes) -
is hand-written in `src/library/include/libraries/mqtt.h`, copied into the
generated header tree by `make library-headers` alongside sfdc's own
output.

**Public API** (8 functions, `struct MqttConnectOpts` for connect-time
options): `MQTT_CreateClient`, `MQTT_Connect`, `MQTT_Publish`,
`MQTT_Subscribe`, `MQTT_GetMessage`, `MQTT_FreeMessage`, `MQTT_Disconnect`,
`MQTT_DeleteClient`. Each client handle is owned by exactly one caller
task - see the threading-model note at the top of `libraries/mqtt.h`.

**Subprocess-per-connection.** `MQTT_CreateClient()` spawns a dedicated
AmigaOS process (`CreateNewProcTags()`) that owns the `bsdsocket.library`
open, the socket, and an unmodified `mqtt_client` instance from
`src/core` - the same struct exercised by the host's fake-transport unit
tests and by the static CLI tools' real on-target runs. The caller's task
and the subprocess talk over two MsgPorts: a synchronous command port
(guiport.c-style request/reply, matching the pattern in the sibling
AmiAuth project) for `Connect`/`Publish`/`Subscribe`/`Disconnect`, and a
caller-owned delivery port that incoming PUBLISHes land on as single-
`AllocVec` `struct MqttMessage`s, retrieved via `MQTT_GetMessage()`.

**QoS 1 outbound with retransmission.** `mqtt_client` itself stays
QoS-0-publish-only and stateless (per `docs/PROTOCOL.md` and the
allocation-free rule above); the one-outstanding-message retry/dedup state
QoS 1 needs lives in the subprocess instead, not the core. Because publish
is synchronous from the caller's point of view, there's at most one
in-flight message to track: the subprocess resends with the DUP flag on a
fixed interval, capped at a few attempts, before giving up.

**Reconnect/backoff.** Opt-in per client via `mco_AutoReconnect` in
`struct MqttConnectOpts`. Once the *first* `MQTT_Connect()` has succeeded,
an unexpected drop (transport error, keepalive timeout - not an explicit
`MQTT_Disconnect()`, not a failed initial connect) makes the subprocess
retry with exponential backoff (1s, 2s, 4s, ... capped at 32s, forever)
and automatically replay every subscription made since connect on each
successful reconnect. `MQTT_Publish()`/`MQTT_Subscribe()` fail fast with
`MQTTERR_NOTCONNECTED` while reconnecting rather than queueing.

## Two build flavours

`mqtt_pub`/`mqtt_sub` (the default, shipped binaries) and
`mqtt_pub-static`/`mqtt_sub-static` (kept alongside them) present an
identical CLI - same ReadArgs templates, same output - but are built from
different code:

| | `mqtt_pub`/`mqtt_sub` | `mqtt_pub-static`/`mqtt_sub-static` |
|---|---|---|
| Links against | `mqtt.library` (`src/amiga/pub_main_lib.c`/`sub_main_lib.c`) | `src/core` + `src/tools` directly (`src/amiga/pub_main.c`/`sub_main.c`) |
| Runtime dependency | `mqtt.library` must be in `LIBS:` | none beyond `bsdsocket.library` |
| Publish QoS | 0 and 1 (real PUBACK round trip) | 0 only |

The default tools exist to dogfood the library end to end (and to give
`mqtt_pub` a real QoS 1 story); the static tools exist for a zero-install
fallback and as a standing comparison point - `tests/net/net-smoke.sh`
runs both in the same Copperline session and checks each reaches the
broker independently.

## Roadmap

- **Phase 1 (done).** Protocol core + `mqtt_pub`/`mqtt_sub` CLI tools.
  QoS 0 publish, QoS 0/1 subscribe (an incoming QoS 1 PUBLISH is
  auto-acknowledged), keepalive scheduling and timeout detection. See
  `docs/PROTOCOL.md` for exact coverage.
- **Phase 2 (done).** `mqtt.library` - see the section above. Also
  shipped: two example programs (`examples/pubexample.c`/`subexample.c`),
  a hand-written autodoc (`src/library/mqtt.doc`), and the CLI tools
  relinked against the library (see "Two build flavours"). What was
  flagged as the project's main technical risk (no AmigaOS shared-library
  scaffolding existed anywhere in this codebase or its AmiAuth sibling)
  turned out to need nothing exotic - see the section above for what was
  actually involved.
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
| `make broker-smoke` | The static host CLI tools against a real Mosquitto - real MQTT wire protocol, real TCP, just not on m68k. |
| `make test-target` | The codec compiled for a real big-endian 68020, under Copperline, with no networking involved. |
| `make net-smoke` | Both `mqtt_pub` flavours (library-linked and static) on real m68k reaching a real Mosquitto via Copperline's HostSocket board in one boot - see `tests/net/README.md`. |
| `make library-smoke` | `mqtt.library` opens/closes cleanly on-target and is findable by `Version`. |
| `make library-net-smoke` | The full library API - connect, publish, subscribe, receive, disconnect - against a real Mosquitto, on real m68k. |
| `make library-reconnect-smoke` | `mco_AutoReconnect`: kills and restarts the real broker mid-run and checks the client recovers unaided, including a post-outage message. |
| `make example-smoke` | The two example programs (`examples/`) build against the library headers and run correctly on-target. |
| `make volamos-*` | Fast local counterparts of the above (milliseconds vs Copperline's ~10s boot) wherever volamos supports the AmigaOS calls involved - not a CI/release substitute, see each script's own header comment for what it can't cover (e.g. volamos has no `CreateNewProc`, so it can only exercise the static CLI build, not the library's subprocess model). |

`make net-smoke`'s design is worth calling out: an earlier sketch assumed a
real guest TCP/IP stack (Roadshow/AmiTCP) on a user-supplied Workbench
image, which would have made the test local-only and gated behind
machine-specific assets. Copperline's bundled HostSocket board
(`[hostsocket] net = "host"`) removes that need entirely - the guest's
`bsdsocket.library` autoboots from the board's own ROM code, delegating
socket calls straight to the host - so this test needs nothing beyond
`copperline` and `mosquitto` on `PATH` and runs in CI as well as locally.
