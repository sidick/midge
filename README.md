# midge

A native MQTT 3.1.1 client for classic AmigaOS.

midge puts your Amiga on a modern smart-home network (Home Assistant,
Mosquitto, EMQX) — as a display/control surface and as a publisher of its
own telemetry. It ships as command-line tools, a shared library
(`mqtt.library`) any other AmigaOS program can call, and — eventually — a
ReAction dashboard application.

> **Status:** the protocol core, `mqtt_pub`/`mqtt_sub` CLI tools,
> `mqtt.library` (subprocess-per-connection, QoS 0/1, MsgPort dispatch,
> opt-in auto-reconnect with backoff), and TLS via AmiSSL (`mco_TLS`,
> optional private-CA trust) are implemented, cross-build to real AmigaOS
> binaries, and are verified end-to-end against a real Mosquitto broker on
> real 68020 codegen via Copperline in CI. Not yet tagged for release. A
> ReAction dashboard with Home Assistant MQTT discovery is next — see the
> [issue tracker](https://github.com/sidick/midge/issues) for what's
> planned (`docs/ARCHITECTURE.md`'s own roadmap section was retired once
> it matched shipped history).

## AI-assisted development

Be aware: **midge was written largely by an AI coding agent** (Anthropic's
Claude, via Claude Code), working under human direction. The scope,
architectural decisions, and on-target verification were human-directed and
reviewed; most of the code itself was AI-generated.

The protocol codec is checked against byte-exact MQTT 3.1.1 vectors and
tested against real brokers (host and on-target); the full source is
BSD-licensed and open for review.

## Why

No maintained MQTT implementation exists for classic AmigaOS — no library,
no CLI, no GUI client. MQTT is a good fit for the platform: tiny framing
overhead, a single long-lived TCP connection, no polling. The library-first
design means the value compounds — once `mqtt.library` exists, any AmigaOS
program can add MQTT support with a handful of calls, not a protocol
implementation of its own.

## Features

- **`mqtt_pub`/`mqtt_sub`** — dependency-free-feeling CLI tools with
  standard `ReadArgs` templates (`mqtt_pub HOST ... TOPIC ... MESSAGE ...`).
  Two build flavours ship side by side: the default tools are backed by
  `mqtt.library` (QoS 0 and 1 publish); `mqtt_pub-static`/`mqtt_sub-static`
  link the protocol code directly with no library install required (QoS 0
  publish only).
- **`mqtt.library`** — a real AmigaOS shared library (`.sfd`-described,
  `sfdc`-generated headers/stubs, `libinit.o`) exposing an 8-function async
  API: create a client, connect, publish, subscribe, poll for incoming
  messages, disconnect. Each client handle is backed by its own dedicated
  subprocess owning the `bsdsocket.library` connection, so bsdsocket's
  per-task handle restriction is invisible to callers. QoS 1 publish
  retransmits with the DUP flag until acknowledged; an opt-in
  `mco_AutoReconnect` mode reconnects with exponential backoff and
  auto-resubscribes after an unexpected drop. Two example programs and a
  hand-written autodoc ship in the release archive's `developer/` tree.
- **TLS via AmiSSL** — opt-in (`TLS`/`TLSINSECURE`/`CAFILE` on the Amiga
  tools, `-s`/`-S`/`-c` on the host builds), certificate and hostname
  verification on by default, with an optional private-CA trust anchor for
  self-hosted brokers. The statically linked tools have no AmiSSL
  dependency and don't support it.
- **Portable, testable core** — the packet codec and connection state
  machine are plain C99 with zero OS dependencies, so protocol-level tests
  run on the host with no emulator.

## Design principles

- **68020 is the baseline target** (`-m68020`) — this differs from sibling
  Amiga projects that target plain 68000; a good reason is needed to raise
  it further, not to lower it.
- **Strict layering** — `src/core/` is portable C99 with zero OS/libc
  dependencies beyond C99 itself (no sockets, no timers, no allocation —
  caller buffers only). All platform code lives in `src/host/`/`src/amiga/`;
  the transport vtable (`src/core/mqtt_transport.h`) is the only seam
  between them - TLS (`src/host/transport_openssl.c`,
  `src/amiga/transport_amissl.c`) is exactly this: another transport
  implementation, no change to the client state machine.
- **Real on-target verification, not just host tests** — every networking
  claim is proven against a real broker on real 68020 codegen (Copperline's
  HostSocket board), not just mocked. `volamos` gives a much faster local
  loop for the same checks where it supports the AmigaOS calls involved.
- **QoS 2 is deliberately out of scope** — see
  [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for why.

## Building

    make test                 # host unit tests (codec + client state machine)
    make cli                  # native mqtt_pub/mqtt_sub -> build/*-host
    make broker-smoke         # host CLIs against a local Mosquitto
    make m68k-docker          # AmigaOS mqtt_pub/mqtt_sub + -static variants
    make library              # AmigaOS mqtt.library -> build/mqtt.library
    make library-net-smoke    # full library API vs a real broker, on-target
    make net-smoke            # both mqtt_pub flavours vs a real broker, on-target
    make dist                 # the Aminet release archive

The core is portable C99, so `test`/`cli` build with any host compiler. CI
(`.github/workflows/ci.yml`) runs the full matrix — host tests, lint,
on-target codec/library/networking checks against a real Mosquitto — on
every push.

## Documentation

**User documentation lives at
[sidick.github.io/midge](https://sidick.github.io/midge/)** — installation,
the CLI reference, and the `mqtt.library` guide and API reference. Its
source is [`userdocs/`](userdocs/) in this repository, and the same pages
become the `midge.guide` shipped in the release archive.

Developer-facing design notes live in `docs/`:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — module layout, the
  transport vtable seam, `mqtt.library`'s subprocess/MsgPort design, and
  the phase roadmap.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — MQTT 3.1.1 coverage, QoS scope,
  and keepalive/session behaviour.

## Toolchain

C via bebbo's `amiga-gcc`, GitHub Actions CI running host unit tests, an
on-target Copperline codec/library/networking suite against a real
Mosquitto broker, and lint; Aminet packaging via `aminet-release-action`.

## License

BSD 2-Clause. See [`LICENSE`](LICENSE).
