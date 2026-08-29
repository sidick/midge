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
| `CLIENTID` | MQTT client identifier (default generated). |
| `USER` / `PASSWORD` | Broker credentials, if required. |
| `KEEPALIVE` | Keepalive interval in seconds (default 60). |
| `RETAIN` | Set the broker's retained flag on this message. |
| `VERBOSE` | Print connection and protocol detail to stdout. |

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
| `CLIENTID` | MQTT client identifier (default generated). |
| `USER` / `PASSWORD` | Broker credentials, if required. |
| `KEEPALIVE` | Keepalive interval in seconds (default 60). |
| `COUNT` | Exit after receiving this many messages. |
| `VERBOSE` | Print connection and protocol detail to stdout. |

Example:

```
mqtt_sub HOST 192.168.1.10 TOPIC home/#
```
