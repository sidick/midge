# midge

midge is a native MQTT 3.1.1 client for classic AmigaOS, aimed at putting
your Amiga on a modern smart-home network (Home Assistant, Mosquitto, EMQX)
as both a display/control surface and a publisher of its own telemetry.

This first release ships the command-line tools `mqtt_pub` and `mqtt_sub`.
A shared library (`mqtt.library`), TLS support, and a ReAction dashboard
application are planned for future releases.

## Requirements

- AmigaOS 3.1+ (3.2 is the reference platform), 68020 or better.
- A TCP/IP stack providing `bsdsocket.library` (Roadshow, AmiTCP, Miami, or
  an emulator-provided stack).

## Quick start

```
mqtt_sub HOST 192.168.1.10 TOPIC home/#
mqtt_pub HOST 192.168.1.10 TOPIC home/amiga/hello MESSAGE "hello from midge"
```

See the [CLI Reference](CLI-Reference.md) for the full argument list.
