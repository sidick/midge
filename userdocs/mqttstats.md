# mqttstats

`mqttstats` is a headless AmigaOS Commodity that publishes system telemetry
(uptime, chip RAM free, fast RAM free, CPU model) to a Home Assistant broker
over MQTT, with full [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)
so the sensors appear in Home Assistant automatically - no manual entity
configuration needed on the HA side.

I2C/external sensor support is planned as a follow-up and not part of this
release - see the project's GitHub issues.

## What it does

Once running, `mqttstats` connects to your broker and publishes, every
`INTERVAL` seconds (60 by default):

| Sensor | Unit | Notes |
|---|---|---|
| Uptime | seconds | Time since the Amiga's last reset (via `timer.device`), not since `mqttstats` started. |
| Chip RAM free | bytes | `AvailMem(MEMF_CHIP)`. |
| Fast RAM free | bytes | `AvailMem(MEMF_FAST)`. |
| CPU model | - | `68020`/`68030`/`68040`/`68060`, detected from `AttnFlags`. |

It also publishes an availability topic (`online` while running, `offline`
on a clean shutdown) and registers a Home Assistant MQTT Discovery config
for each sensor on connect, so Home Assistant creates matching entities
under one device per Amiga without any `configuration.yaml` editing.

## Running it

`mqttstats` has no window and no menu - by design, it never opens one. Its
only control surface is [Commodities Exchange](https://en.wikipedia.org/wiki/AmigaOS#Commodities_Exchange):
once running, use Exchange to Disable, re-enable, or Kill it. There is
nothing else to interact with.

Everything is configured through ToolTypes (or, run from a Shell, the
identical `KEYWORD=VALUE` arguments) - there are no command-line switches
to memorise. Set them via the icon's Information window (Workbench) or on
the command line the same way you'd configure any other Amiga tool.

| ToolType | Meaning |
|---|---|
| `HOST` | Broker hostname or IP address (**required**). |
| `PORT` | Broker TCP port (default 1883, or 8883 if `TLS` is set). |
| `CLIENTID` | MQTT client identifier (default `midge-stats`). Also the default source for `NODEID` - give each Amiga a unique one if you run `mqttstats` on more than one machine against the same broker. |
| `DEVICENAME` | Friendly name shown in Home Assistant for this Amiga's device (default `Amiga`). |
| `NODEID` | Overrides the MQTT topic/discovery id derived from `CLIENTID` (letters, digits, `_`, `-` only; other characters are replaced with `_`). Only needed if you want topics that don't track `CLIENTID`. |
| `USER` / `PASSWORD` | Broker credentials, if required. |
| `TLS` | Connect over TLS via AmiSSL - see [TLS on the Amiga](CLI-Reference.md#tls-on-the-amiga) (same requirements and caveats as `mqtt_pub`/`mqtt_sub`). |
| `TLSINSECURE` | Connect over TLS but skip certificate verification (implies `TLS`). Testing only. |
| `CAFILE` | Trust an additional CA certificate (PEM file). Ignored without `TLS`, and ignored if `TLSINSECURE` is also given. |
| `INTERVAL` | Publish interval in seconds (default 60). |
| `CX_PRIORITY` | Commodities Exchange broker priority (default 0) - only matters if you're stacking many Commodities and care about their relative activation order. |

**Multiple Amigas on one broker**: give each machine's icon its own
`CLIENTID` (and, if you want, `DEVICENAME`) - that's also what determines
the MQTT topic prefix and Home Assistant device identity, so two Amigas
with the same `CLIENTID` would collide on both.

### Running from Workbench / WBStartup

`mqttstats` ships with its own icon (`mqttstats.info` in the archive,
alongside the binary) - copy both together wherever you put `mqttstats`.
Open its Information window (Workbench's Icons menu, or right-click on
some Workbench versions) and set at least `HOST` in the Tool Types list;
the icon ships with a few common ones already listed as comments (shown
grayed out, in parentheses) as a reminder of what's available - edit them
in place to activate. Then either double-click the icon to run it, or drag
it into `WBStartup:` to have it launch silently every boot.

Either way, nothing appears on screen - if `HOST` is missing or a library
fails to open, `mqttstats` quits immediately with no visible indication
(diagnostics go to the serial port only, for anyone debugging under an
emulator or with a serial cable attached - see
[CLI Reference](CLI-Reference.md) for the general shape of Amiga tool
diagnostics). Check Home Assistant for the expected entities to confirm
it's actually connected.

### Running from a Shell

```
mqttstats HOST 192.168.1.10 CLIENTID a1200-office
```

Useful for a quick test before committing to ToolTypes and WBStartup - the
same required/optional arguments apply, just as `KEYWORD VALUE` pairs
instead of icon ToolTypes.

## Home Assistant setup

Nothing needs configuring on the Home Assistant side beyond
[MQTT discovery being enabled](https://www.home-assistant.io/integrations/mqtt/#configuration)
(the default for HA's own MQTT integration). Once `mqttstats` connects:

1. Each of the four sensors above appears automatically under a device
   named after `DEVICENAME`, grouped with any other `mqttstats` sensors
   from the same Amiga.
2. The device shows as unavailable if `mqttstats` disconnects
   (broker down, network dropped, `mqttstats` killed via Exchange) and
   available again once it reconnects or restarts.
3. To stop reporting a given Amiga, Kill its `mqttstats` via Commodities
   Exchange (or just don't relaunch it after a reboot) - Home Assistant
   marks its entities unavailable rather than removing them; delete the
   device from HA's MQTT integration page if you want it gone for good.
