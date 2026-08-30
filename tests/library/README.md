# tests/library/ — on-target mqtt.library smoke tests

`run.sh`, `net-run.sh`, and `reconn-run.sh` (and their `volamos-*`
counterparts) all boot Copperline's bundled AROS Kickstart replacement with
the HostSocket board fitted (see `../net/README.md`'s "Why this needs no
machine-specific assets") - no Kickstart ROM, no Workbench image, nothing
beyond `copperline`/`mosquitto` on `PATH`. They're CI/release gates
(`.github/workflows/ci.yml`).

## TLS smoke test

`tls-run.sh` (`make library-tls-smoke`) is different: it's **local-only**,
not a CI/release gate, because AmiSSL needs things the bundled-AROS boot
doesn't have:

- A real `AmigaOS` `Devs:`/`Libs:` install layout - the `amissl` package's
  own install step assigns `AmiSSL:` to `SYS:Devs/AmiSSL` (where its cert
  store lives), which only exists on a real OS boot.
- A real Kickstart ROM - AmiSSL requires actual AmigaOS 3.0+, not the
  bundled-AROS Kickstart replacement the other tests use.

Neither is something CI can be handed (a Kickstart ROM is Cloanto/Hyperion
licensed media; a full OS install image is large and itself
proprietary-derived) - same reasoning `../copperline/`'s optional ROM and
`../net/`'s original (superseded) Roadshow-image design ran into.

### Building the image

Use [amibake](https://github.com/sidick/amibake) (a separate,
manifest-driven Amiga image builder) to build a real AmigaOS 3.2.2 +
AmiSSL 5.27 image:

```sh
# in a checkout of amibake, with your own licensed AmigaOS 3.2 install
# media + Kickstart ROM under assets/ (see amibake's own docs/limits.md)
cat > manifests/midge-library-tls.toml <<'EOF'
base     = "os3.2.2"
machine  = { cpu = "68020", ram = "chip:1M,fast:8M" }
packages = ["amissl = 5.27"]
output   = ["dir"]
emit     = ["copperline"]
EOF
.venv/bin/amibake resolve manifests/midge-library-tls.toml
.venv/bin/amibake build manifests/midge-library-tls.toml --assets assets
```

This writes `manifests/midge-library-tls/` (the boot image) and
`manifests/midge-library-tls.copperline.toml` (which names the Kickstart
ROM amibake resolved against - `tls-run.sh` re-derives its own machine
config, but that file tells you which ROM path to point
`MIDGE_TLS_KICKSTART` at if it isn't amibake's own default location).

### Running it

```sh
export MIDGE_TLS_AMIGA_IMAGE=/path/to/amibake/manifests/midge-library-tls
export MIDGE_TLS_KICKSTART=/path/to/amibake/assets/roms/kickstart-47.7.rom
make fetch-amissl-sdk   # once, populates AMISSL_SDK_DIR for `make library`
make library-tls-smoke
```

`tls-run.sh` stages `build/libtls` + `build/mqtt.library` into the image
and overwrites its `S/User-Startup` on every run (see the script's own
banner for exactly what it does and why) - point `MIDGE_TLS_AMIGA_IMAGE` at
an image you don't mind that happening to, not a real Workbench install you
use for anything else.

See [tests/copperline/amissl-spike/](../copperline/amissl-spike/) for the
earlier de-risking investigation this test's approach (amibake image,
`AmiSSL:` assign trap, real-hardware CPU-speed caveat) is drawn from -
issue #3 has the full writeup.
