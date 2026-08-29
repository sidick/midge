# midge — project instructions

midge is a native MQTT 3.1.1 client for classic AmigaOS: today the
`mqtt_pub`/`mqtt_sub` CLI tools; the roadmap adds `mqtt.library` (shared
library), TLS via AmiSSL, and a ReAction dashboard with Home Assistant MQTT
discovery. Developer design notes live in `docs/` (start with
`docs/ARCHITECTURE.md`); the original proposal-level rationale is summarised
there too.

## Key project rules

- **Baseline target is a 68020** (`-m68020`), AmigaOS 3.1+ (3.2 is the
  reference platform). This differs from sibling projects like AmiAuth
  (68000) — do not "fix" it down.
- **Layering is strict**: `src/core/` is portable C99 with zero OS or libc-
  beyond-C99 dependencies (no sockets, no timers, no allocation — caller
  buffers only). All platform code lives in `src/host/` and `src/amiga/`;
  shared tool logic in `src/tools/`. The transport vtable
  (`src/core/mqtt_transport.h`) is the only seam between them — TLS lands
  later as another transport, so don't bypass it.
- The core must stay unit-testable on the host (`make test`) and is the
  future body of `mqtt.library` — no statics that would break multiple
  simultaneous connections, no global state.
- Amiga shell stack is small (~4 KB default) — keep large buffers off the
  stack.
- bsdsocket.library handles are per-task; never share a socket across
  processes. Use `WaitSelect()` with `SIGBREAKF_CTRL_C` in the signal mask so
  network waits stay abortable.
- QoS 2 is deliberately out of scope (see `docs/PROTOCOL.md`).

## Build / test

- `make test` — host unit tests; `make cli` — host-native tools;
  `make broker-smoke` — host tools against a local Mosquitto.
- `make m68k-docker` — cross-build Amiga binaries in the CI container.
- `make test-target` — on-target codec test under Copperline (AROS ROM,
  works from a fresh checkout).
- `make net-smoke` — on-target network test: cross-built `mqtt_pub` under
  Copperline's HostSocket board (`bsdsocket.library` backed by a real host
  socket, `net = "host"`) reaching a real Mosquitto on the host. Needs no
  Kickstart ROM or Workbench image (see `tests/net/README.md`) and runs in
  CI as well as locally.
- CI is the five-verb contract (`build` / `test-host` / `test-target` /
  `lint` / `dist`) via `sidick/amiga-workflows@v1` — ci.yml calls those
  Makefile targets by name; keep them working.

## Documentation lives in userdocs/

User-facing documentation is `userdocs/` — published as a versioned MkDocs
Material site (deployed on release tags by `.github/workflows/docs.yml` via
`mike`) and converted to the `midge.guide` shipped in the release archive
(`tools/docs2guide.py`, `make guide`).

**Whenever you change user-visible behaviour, update the affected
`userdocs/` pages in the same PR** — CLI templates/arguments and their
output, settings, error messages worth documenting, OS/library
requirements. If a change is purely internal, no docs edit is needed — but
say so explicitly. Adding a page means updating the `nav` in `mkdocs.yml`
AND `PAGES` in `tools/docs2guide.py`. Keep pages within docs2guide's
Markdown subset (headings, bold, inline/fenced code, pipe tables, lists,
blockquotes, links) so the AmigaGuide stays faithful — bullet lists in
particular must use column-0 `- ` markers with exactly 2-space
continuation, never nested/deeper indentation (4+ spaces reads as an
indented code block to `docs2guide.py`'s parser).

`userdocs/mqtt-library-reference.md` is the one generated page: it's built
from `src/library/mqtt.doc` (the API autodoc, itself hand-maintained) by
`tools/autodoc2md.py`. If you change `mqtt.doc`, run `make api-reference`
and commit the regenerated page in the same PR — don't hand-edit it.

## Releases

Tag-driven: bump `MIDGE_VERSION` + `MIDGE_VERSION_DATE` in `src/version.h`
and `Version:` in `midge.readme` via a release PR, then push the matching
`vX.Y` tag. `scripts/verify-version.sh` refuses mismatches; `make dist`
verifies the built binaries embed the current `$VER`.
