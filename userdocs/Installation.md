# Installation

## Requirements

- AmigaOS 3.1+ (3.2 is the reference platform), 68020 or better.
- A TCP/IP stack providing `bsdsocket.library` (Roadshow, AmiTCP, Miami, or
  an emulator-provided stack).

## Unpacking

The Aminet archive is a single drawer, `midge`, containing:

```
midge/
  mqtt_pub, mqtt_sub                  the default tools (need mqtt.library)
  mqtt_pub-static, mqtt_sub-static    statically linked, no library needed
  midge.guide                         this documentation, for MultiView/AmigaGuide
  midge.readme, LICENSE
  libs/
    mqtt.library                      the shared library
  developer/
    mqtt.doc, mqtt_lib.sfd, fd/, clib/, proto/, inline/, libraries/, examples/
                                       for programs written against mqtt.library
```

Unpack it (`LhA x midge.lha` from Shell, or via a Workbench archiver) and
copy the `midge` drawer wherever you like - `Work:midge`, a drawer under
`SYS:Utilities/`, anywhere on your `Path`. Nothing needs to live in a
specific location.

## Installing mqtt.library (needed for the default tools)

`mqtt_pub`/`mqtt_sub` - the tools you get by just typing their names - are
built against `mqtt.library` and need it present at `LIBS:` to run:

```
Copy midge/libs/mqtt.library LIBS:
```

That's the whole install step: any program that opens `mqtt.library`
(including `mqtt_pub`/`mqtt_sub` themselves) then just works, the same as
any other shared library. There is nothing to configure and no reboot
required - `OpenLibrary()` finds it the next time something asks for it.

If you'd rather not install anything, use `mqtt_pub-static`/
`mqtt_sub-static` instead - see
[Two build flavours](CLI-Reference.md#two-build-flavours) for exactly what
that trade-off costs you (QoS 1 publish support).

## Verifying it worked

```
Version LIBS:mqtt.library FULL
```

should print the library's version string once it's installed. Then try
the [quick start](index.md#quick-start) commands against a broker on your
network.

## For developers

If you're writing your own AmigaOS program against `mqtt.library` rather
than just running the CLI tools, see
[mqtt.library](mqtt-library.md#installing) for the same install step from
a caller's point of view, and the
[API Reference](mqtt-library-reference.md) for every function. The
`developer/` directory in the archive has everything needed to link
against it (headers, `.fd`, and two example programs).
