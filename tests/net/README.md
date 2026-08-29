# tests/net/ — on-target networking smoke test

`net-smoke.sh` proves the real Amiga transport (`src/amiga/transport_bsdsocket.c`,
`bsdsocket.library`) end to end: a cross-built m68k `mqtt_pub` runs under
Copperline and reaches a real Mosquitto listening on the host.

This is the one midge test that exercises actual MQTT traffic over the
Amiga socket codepath rather than a fake transport or a host build — see
`tests/test_client.c` for the state-machine unit tests and
`tests/copperline/` for the on-target *codec* self-test (no networking).

## Why this needs no machine-specific assets

An earlier design for this test assumed a real guest TCP/IP stack
(Roadshow/AmiTCP) on a user-supplied Workbench image, which would have made
it local-only and gated behind a `.env` file (mirroring how
`tests/copperline/` handles an optional licensed Kickstart ROM).

Instead this uses Copperline's bundled **HostSocket** board
(`[hostsocket] net = "host"`, see Copperline's
`docs/guide/configuration.md`): the guest's `bsdsocket.library` autoboots
from the board's own ROM code — on the bundled AROS Kickstart replacement,
same as `tests/copperline/` — and delegates every socket call straight to a
real host OS socket. No Kickstart ROM, no Workbench image, no TCP/IP stack
to install. That means this test needs nothing beyond `copperline` and
`mosquitto` on `PATH`, and runs in CI (see `.github/workflows/ci.yml`'s
`net-smoke` job) as well as locally.

## Running it

```
make net-smoke
```

Cross-builds `mqtt_pub` (m68k) and the host `mqtt_sub-host` (used as the
observer that confirms the message actually arrived), starts a scratch
Mosquitto, boots `mqtt_pub` under Copperline (staged as an FFS boot volume,
same pattern as `tests/copperline/`) with the HostSocket board fitted, and
checks the message was received.

For a much faster local loop while developing (no emulated boot - about a
second instead of Copperline's ~10s AROS boot), `make volamos-smoke` runs
the same check under `volamos` (an API-level AmigaOS runtime; `--net`
enables its `bsdsocket.library` support) instead. It's not a substitute for
`make net-smoke` as a CI/release gate - volamos traps library calls at the
API boundary rather than running real 68020 codegen through a booted
Kickstart, and it isn't installed in CI - but it's a good first check while
iterating or bisecting a networking bug.
