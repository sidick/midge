# CLI Reference

## Two build flavours

The release archive ships two builds of each tool, sharing the same
command-line syntax below:

- `mqtt_pub` / `mqtt_sub` - the default build. Backed by
  [mqtt.library](mqtt-library.md), so `libs/mqtt.library` from the archive
  must be copied to `LIBS:` first. Supports `QOS` 0 and 1 on publish.
- `mqtt_pub-static` / `mqtt_sub-static` - statically linked builds that
  need nothing beyond `bsdsocket.library` (no library install required).
  `mqtt_sub-static` subscribes at `QOS` 0 or 1 same as the default build;
  `mqtt_pub-static` only supports `QOS` 0 on publish.

## mqtt_pub

Publish a single message to a broker topic and exit.

```
mqtt_pub HOST/A,PORT/N/K,TOPIC/A,MESSAGE/K,FILE/K,QOS/N/K,CLIENTID/K,USER/K,
         PASSWORD/K,KEEPALIVE/N/K,RETAIN/S,VERBOSE/S
```

| Argument | Meaning |
|---|---|
| `HOST` | Broker hostname or IP address (required). |
| `PORT` | Broker TCP port (default 1883). |
| `TOPIC` | Topic to publish to (required). |
| `MESSAGE` | Payload text. Mutually exclusive with `FILE`. |
| `FILE` | Read the payload from a file instead of `MESSAGE`. |
| `QOS` | 0 or 1 (default 0). |
| `CLIENTID` | MQTT client identifier. If omitted, the CONNECT packet carries an empty client id, asking the broker to assign one; some brokers refuse an empty client id (CONNACK return code 2) unless clean-session behaviour allows it. |
| `USER` / `PASSWORD` | Broker credentials, if required. |
| `KEEPALIVE` | Keepalive interval in seconds (default 60). |
| `RETAIN` | Set the broker's retained flag on this message. |
| `VERBOSE` | Print connection and protocol detail to stdout. |
| `TLS` | Connect over TLS via AmiSSL, with certificate and hostname verification on. Default build only - see [TLS on the Amiga](#tls-on-the-amiga). When no `PORT` is given, the default becomes 8883 instead of 1883. |
| `TLSINSECURE` | Connect over TLS but skip certificate verification (implies `TLS`). For testing against self-signed or otherwise untrusted brokers only. |

Example:

```
mqtt_pub HOST 192.168.1.10 TOPIC home/amiga/hello MESSAGE "hello from midge"
```

## mqtt_sub

Subscribe to a topic filter and print incoming messages until interrupted
with Ctrl-C.

```
mqtt_sub HOST/A,PORT/N/K,TOPIC/A,QOS/N/K,CLIENTID/K,USER/K,PASSWORD/K,
         KEEPALIVE/N/K,COUNT/N/K,VERBOSE/S
```

| Argument | Meaning |
|---|---|
| `HOST` | Broker hostname or IP address (required). |
| `PORT` | Broker TCP port (default 1883). |
| `TOPIC` | Topic filter to subscribe to (required); wildcards `+`/`#` allowed. |
| `QOS` | 0 or 1 (default 0). |
| `CLIENTID` | MQTT client identifier. If omitted, the CONNECT packet carries an empty client id, asking the broker to assign one; some brokers refuse an empty client id (CONNACK return code 2) unless clean-session behaviour allows it. |
| `USER` / `PASSWORD` | Broker credentials, if required. |
| `KEEPALIVE` | Keepalive interval in seconds (default 60). |
| `COUNT` | Exit after receiving this many messages. |
| `VERBOSE` | Print connection and protocol detail to stdout. |
| `TLS` | Connect over TLS via AmiSSL, with certificate and hostname verification on. Default build only - see [TLS on the Amiga](#tls-on-the-amiga). When no `PORT` is given, the default becomes 8883 instead of 1883. |
| `TLSINSECURE` | Connect over TLS but skip certificate verification (implies `TLS`). For testing against self-signed or otherwise untrusted brokers only. |

Example:

```
mqtt_sub HOST 192.168.1.10 TOPIC home/#
```

## Host development builds

The repo also builds host-native `mqtt_pub-host` / `mqtt_sub-host` (via
`make cli`), used for development and by the CI broker smoke tests. They
take getopt-style flags mirroring the Amiga arguments above (`-h HOST`,
`-p PORT`, `-t TOPIC`, and so on), plus two flags that only exist on the
host builds so far:

| Flag | Meaning |
|---|---|
| `-s` | Enable TLS, with certificate and hostname verification on, checked against the system trust store. When no `-p` is given, the default port becomes 8883 instead of 1883. |
| `-S` | Enable TLS but skip certificate verification. Intended only for testing against self-signed or otherwise untrusted brokers. |

TLS is opt-in and off by default everywhere in midge.

## TLS on the Amiga

The default (mqtt.library-linked) `mqtt_pub`/`mqtt_sub` support TLS via
the `TLS`/`TLSINSECURE` switches above. Requirements:

- [AmiSSL](https://github.com/jens-maus/amissl) 5.x installed - its
  installer sets up `LIBS:amisslmaster.library`, the CPU-tier
  `LIBS:AmiSSL/` library, and the `AmiSSL:` assign the cert store is read
  through. All three are needed; without them `TLS` fails with a connect
  error (and a missing `AmiSSL:` assign in particular will make AmigaOS
  ask for the volume by requester).
- This build of `mqtt.library` compiled with AmiSSL support (release
  builds are; a from-source build needs `make fetch-amissl-sdk` first -
  see the Makefile).

The statically linked `mqtt_pub-static`/`mqtt_sub-static` have no AmiSSL
support and reject `TLS` outright rather than silently connecting in
plaintext.

### A note on TLS and CPU speed

Software TLS is CPU-intensive, and de-risking work for the Amiga-side
AmiSSL transport found that a genuinely stock, unaccelerated 68020
(around 14MHz, e.g. an A1200's 68EC020) sits right at the edge of a
timing-sensitive failure: the handshake completes, but the connection can
then fail intermittently on the write that follows it. A modest speed
bump - any real accelerator, or a 68030 or better - clears this
reliably. This isn't specific to midge: [AmiSSL's own maintainer has
reached the same conclusion](https://github.com/jens-maus/amissl/issues/111)
for other software - "the Amiga can't keep up with modern SSL" at stock
clock speeds. Once TLS ships on the Amiga side, expect it to work best on
an accelerated machine, and to occasionally need a retry on genuinely
stock hardware.
