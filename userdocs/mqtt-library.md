# mqtt.library

mqtt.library gives any AmigaOS program the same MQTT 3.1.1 client that
powers `mqtt_pub`/`mqtt_sub`, as an ordinary shared library other software
can call. Use it to add MQTT publish/subscribe to your own AmigaOS
program without writing any protocol code.

## Requirements

- AmigaOS 3.1+ (3.2 is the reference platform), 68020 or better.
- A TCP/IP stack providing `bsdsocket.library` (Roadshow, AmiTCP, Miami, or
  an emulator-provided stack) - the same requirement as the CLI tools.

## Installing

Copy `libs/mqtt.library` from the release archive to `LIBS:`. Programs
that use it then just `OpenLibrary("mqtt.library", 0)` like any other
shared library - there is nothing else to configure.

## Architecture, in brief

Each client handle you create is backed by its own dedicated AmigaOS
subprocess, which owns the `bsdsocket.library` connection and drives it.
Calls that talk to that subprocess (`MQTT_Connect`, `MQTT_Publish`,
`MQTT_Subscribe`, `MQTT_Disconnect`) block until the subprocess has
finished that step and return its result - there is no callback or event
model to wire up. A handle belongs to the task that created it: every
call on it, including fetching and freeing messages, must be made from
that same task.

## A short example

This is trimmed from `developer/examples/pubexample.c` in the release
archive - see that file (and its companion `subexample.c`, which
subscribes and polls for incoming messages) for a complete, buildable
program.

```c
#include <libraries/mqtt.h>
#include <proto/mqtt.h>

struct Library *MqttBase;

MqttBase = OpenLibrary((STRPTR) "mqtt.library", 0);
if (!MqttBase) {
    /* not installed - tell the user to copy it to LIBS: */
}

struct MqttConnectOpts opts;
memset(&opts, 0, sizeof(opts));
opts.mco_ClientID = (STRPTR) "my-amiga-app";
opts.mco_KeepAlive = 60;
opts.mco_CleanSession = TRUE;

APTR client = MQTT_CreateClient((STRPTR) "192.168.1.10", 1883, &opts);
if (MQTT_Connect(client) == 0) {
    MQTT_Publish(client, (STRPTR) "home/amiga/hello",
                 "hello from midge", 17, FALSE, 1); /* QoS 1 */
    MQTT_Disconnect(client);
}
MQTT_DeleteClient(client);
CloseLibrary(MqttBase);
```

Every mqtt.library caller must declare `MqttBase` itself - unlike a
library's own base, there is no automatic open for a library the linker
doesn't know about.

## The API

Eight functions cover the whole surface:

| Function | Purpose |
|---|---|
| `MQTT_CreateClient(host, port, opts)` | Allocate a client handle and its connection subprocess. Does not touch the network yet. |
| `MQTT_DeleteClient(client)` | Disconnect if needed, tear down the subprocess, free everything. |
| `MQTT_Connect(client)` | Open the TCP connection and perform the MQTT handshake. |
| `MQTT_Publish(client, topic, payload, len, retain, qos)` | Publish a message at QoS 0 or 1. |
| `MQTT_Subscribe(client, filter, qos)` | Subscribe to a topic filter (`+`/`#` wildcards allowed). |
| `MQTT_GetMessage(client)` | Poll for the next queued incoming message; returns `NULL` if none is waiting. |
| `MQTT_FreeMessage(client, msg)` | Free a message returned by `MQTT_GetMessage()`. |
| `MQTT_Disconnect(client)` | Close the connection; the handle itself stays valid. |

Full parameter-by-parameter documentation - the same content as the
autodoc entries below - lives in `developer/mqtt.doc` in the release
archive, readable with AmigaDOS's standard autodoc conventions.

## QoS semantics

QoS 0 and QoS 1 only; QoS 2 is deliberately out of scope, exactly as for
`mqtt_pub`/`mqtt_sub`.

- **QoS 0 publish** is fire-and-forget: `MQTT_Publish()` returns as soon
  as the packet has been written to the transport.
- **QoS 1 publish** blocks until the broker's `PUBACK` arrives,
  retransmitting roughly every 5 seconds (up to 3 retransmits, ~15-20
  seconds worst case) before giving up with `MQTTERR_TIMEOUT`.
- **Subscribe** blocks until the broker's `SUBACK` grants the
  subscription, or returns `MQTTERR_REFUSED`/`MQTTERR_TIMEOUT`.
- Because each handle's commands are synchronous, at most one QoS 1
  publish is ever outstanding per handle - use a second client handle
  (on a second task) for concurrent publishes.

## Auto-reconnect

Setting `mco_AutoReconnect = TRUE` in the `MqttConnectOpts` passed to
`MQTT_CreateClient()` changes what happens after an *unexpected*
connection drop (a transport error, or a missed keepalive) once
`MQTT_Connect()` has already succeeded once:

- The client's own subprocess reconnects automatically, with exponential
  backoff (1s, 2s, 4s, ... capped at 32 seconds, retried forever), and
  re-issues every subscription made since the last `MQTT_Connect()` on
  each successful reconnect.
- While a reconnect is in progress, `MQTT_Publish()` and
  `MQTT_Subscribe()` fail fast with `MQTTERR_NOTCONNECTED` rather than
  queueing; `MQTT_GetMessage()` is unaffected and keeps returning
  anything already queued.
- An explicit `MQTT_Disconnect()` or a failed *initial* `MQTT_Connect()`
  does not trigger this - only a drop after a successful connect does.

The default (a zeroed `MqttConnectOpts`, or `mco_AutoReconnect = FALSE`)
is today's plain behaviour: an unexpected drop leaves the client
disconnected until the caller makes a fresh `MQTT_Connect()` call.

## Error codes

Every function that returns a status returns 0 on success and a negative
code on failure:

| Code | Value | Meaning |
|---|---|---|
| `MQTTERR_OK` | 0 | Success. |
| `MQTTERR_NOMEM` | -200 | Out of memory creating the handle, a message port, or the subprocess. |
| `MQTTERR_NOSTACK` | -201 | `CreateNewProcTags()` failed to start the connection subprocess. |
| `MQTTERR_NOTCONNECTED` | -202 | Called before `MQTT_Connect()` succeeded, after `MQTT_Disconnect()`, or while auto-reconnect is mid-reconnect. |
| `MQTTERR_TIMEOUT` | -203 | QoS 1 publish: no `PUBACK` within the retry budget. Subscribe: no `SUBACK` within ~10 seconds. |
| `MQTTERR_REFUSED` | -204 | Subscribe: the broker's `SUBACK` refused the subscription. |

Other negative values are passed straight through from the portable MQTT
codec below the library (a malformed packet, a rejected `CONNECT`, and so
on) - any negative return is a failure, 0 is always success.

## Full reference

The release archive's `developer/` directory has everything needed to
build against mqtt.library from a third-party project:

- `mqtt_lib.sfd` and the generated `fd/mqtt_lib.fd` - the function
  definition files.
- `proto/mqtt.h`, `inline/mqtt.h`, `clib/mqtt_protos.h` - the caller-side
  headers; `#include <proto/mqtt.h>` after `<libraries/mqtt.h>` is all a
  GCC-based project needs.
- `libraries/mqtt.h` - the public data types (`struct MqttConnectOpts`,
  `struct MqttMessage`, the `MQTTERR_*` codes).
- `mqtt.doc` - the full autodoc, one entry per function plus a background
  overview.
- `examples/pubexample.c` and `examples/subexample.c` - complete,
  buildable example programs.
