# midge

midge is a native MQTT 3.1.1 client for classic AmigaOS, aimed at putting
your Amiga on a modern smart-home network (Home Assistant, Mosquitto, EMQX)
as both a display/control surface and a publisher of its own telemetry.

This release ships the command-line tools `mqtt_pub` and `mqtt_sub`,
`mqttstats` (a headless Commodity publishing Amiga telemetry to Home
Assistant via MQTT discovery - see [mqttstats](mqttstats.md)), and
`mqtt.library`, a shared library exposing the same MQTT client to other
AmigaOS programs, with optional TLS via AmiSSL. A ReAction dashboard
application is planned for a future release.

## Requirements

- AmigaOS 3.1+ (3.2 is the reference platform), 68020 or better.
- A TCP/IP stack providing `bsdsocket.library` (Roadshow, AmiTCP, Miami, or
  an emulator-provided stack).
- For TLS: [AmiSSL](https://github.com/jens-maus/amissl) 5.x, installed
  separately - see [Installation](Installation.md#installing-amissl-needed-for-tls).

See [Installation](Installation.md) for unpacking the archive and the
`mqtt.library` install step the default tools need.

## Quick start

```
mqtt_sub HOST 192.168.1.10 TOPIC home/#
mqtt_pub HOST 192.168.1.10 TOPIC home/amiga/hello MESSAGE "hello from midge"
```

See the [CLI Reference](CLI-Reference.md) for the full argument list, or
[mqtt.library](mqtt-library.md) if you're writing your own AmigaOS
program against the MQTT client directly.
