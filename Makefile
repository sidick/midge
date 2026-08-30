# midge build.
#
#   make test        build and run the host-side unit tests (default)
#   make cli         build the host-native mqtt_pub/mqtt_sub tools
#   make broker-smoke  run the host tools against a local Mosquitto
#   make broker-tls-smoke  run the host tools' TLS support against a local Mosquitto
#   make m68k        cross-build the Amiga binaries (needs amiga-gcc on PATH)
#   make m68k-docker cross-build inside the CI container (no local toolchain)
#   make net-smoke   on-target network test (Copperline HostSocket; no ROM/WB needed)
#   make volamos-smoke  same check via volamos - faster local loop, not a CI substitute
#   make volamos-test-target  codec self-test via volamos - faster local loop
#   make examples     build the example mqtt.library caller programs (m68k)
#   make library-smoke  on-target mqtt.library OpenLibrary/CloseLibrary smoke test
#   make volamos-library-smoke  same check via volamos - faster local loop
#   make library-net-smoke  on-target mqtt.library end-to-end API test (real broker)
#   make volamos-library-net-smoke  same check via volamos - faster local loop, not a CI substitute
#   make library-reconnect-smoke  on-target mco_AutoReconnect test (broker restart mid-run)
#   make fetch-amissl-sdk  fetch the AmiSSL v5 SDK (needed for Amiga-side TLS support)
#   make library-tls-smoke  on-target mco_TLS test - local-only, needs an amibake image
#   make library-cafile-smoke  on-target mco_CAFile test - local-only, needs an amibake image
#   make clean
#
# The core is portable C99, so `test` and `cli` build with any host compiler.

# --- Host toolchain (tests + native tools) ---
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
CObjINC := -Isrc/core

# Host TLS transport (src/host/transport_openssl.c) links host OpenSSL
# 1.1+/3.x. Not on the default compiler search path on macOS (Homebrew
# keeps it out of /usr/include and /usr/lib to avoid shadowing the system
# LibreSSL), so go through pkg-config; fall back to plain -lssl -lcrypto for
# hosts where OpenSSL is where the compiler already expects it.
HOST_SSL_CFLAGS ?= $(shell pkg-config --cflags openssl 2>/dev/null)
HOST_SSL_LIBS   ?= $(shell pkg-config --libs openssl 2>/dev/null || echo -lssl -lcrypto)

# Feature-detect OpenSSL rather than requiring it unconditionally: the
# shared amiga-dev CI image (ci/test-host, ci/build - see
# .github/workflows/ci.yml) has no OpenSSL headers and this repo has no way
# to add an apt-get step to those reusable-workflow jobs, so `cli`/`test`
# must still build cleanly there, just without TLS support in that build.
# Same idiom AmiSSL will use for the m68k side (see issue #3's plan) -
# amiauth's own `make diff` target follows the sibling precedent of keeping
# an optional host OpenSSL dependency out of its core build/test verbs
# entirely; this goes one step further and degrades gracefully instead,
# since -s/-S need to be real flags on the shipped mqtt_pub-host/
# mqtt_sub-host binaries (broker-tls-smoke's job installs libssl-dev, or
# gets it for free on a stock ubuntu-latest runner, so it always gets full
# TLS support).
HOST_HAS_TLS := $(shell $(CC) $(HOST_SSL_CFLAGS) -c -include openssl/ssl.h -x c /dev/null -o /dev/null >/dev/null 2>&1 && echo 1)
ifeq ($(HOST_HAS_TLS),1)
HOST_TLS_SRCS   := src/host/transport_openssl.c
HOST_TLS_CFLAGS := $(HOST_SSL_CFLAGS) -DMIDGE_HOST_TLS
HOST_TLS_LIBS   := $(HOST_SSL_LIBS)
endif

# Amiga-side TLS transport (src/amiga/transport_amissl.c) links the AmiSSL
# v5 SDK (headers + libamisslstubs.a) - a proprietary, freely-downloadable
# archive (scripts/fetch-amissl-sdk.sh), not part of the m68k-amigaos-gcc
# toolchain itself. Feature-detected exactly like HOST_HAS_TLS above (same
# rationale: the shared amiga-dev CI image has no AmiSSL SDK, and `library`/
# `m68k` must still build cleanly there without it) - run
# `make fetch-amissl-sdk` first to populate AMISSL_SDK_DIR and get TLS
# support in the next `library`/`m68k` build.
AMISSL_VERSION  ?= 5.27
AMISSL_SDK_DIR  ?= .cache/amissl-sdk/amissl-$(AMISSL_VERSION)/sdk/AmiSSL/Developer
M68K_HAS_AMISSL := $(shell test -f "$(AMISSL_SDK_DIR)/include/openssl/ssl.h" && echo 1)
ifeq ($(M68K_HAS_AMISSL),1)
AMISSL_SRCS   := src/amiga/transport_amissl.c
AMISSL_CFLAGS := -I$(AMISSL_SDK_DIR)/include -DMIDGE_AMIGA_TLS
AMISSL_LIBS   := -L$(AMISSL_SDK_DIR)/lib/AmigaOS3 -lamisslstubs
endif

fetch-amissl-sdk:
	scripts/fetch-amissl-sdk.sh

# --- m68k cross toolchain (Amiga build) ---
# Baseline per the project spec is 68020 (not 68000 - see CLAUDE.md).
M68K_CC     ?= m68k-amigaos-gcc
M68K_CFLAGS ?= -std=c99 -O2 -Wall -Wextra -m68020 -noixemul $(CObjINC) -Isrc -Isrc/amiga -Isrc/tools

# Binaries embed the commit hash in $VER unless this build is exactly on a
# release tag (the tag-driven release workflow always builds from a `vX.Y`
# tag, so "on a tag at all" is a reliable proxy for "this is the release
# build"). Computed on the host so the docker targets don't need git inside
# the container - passed through as plain make variables instead.
GIT_HASH   ?= $(shell git rev-parse --short HEAD 2>/dev/null)
GIT_ON_TAG ?= $(shell git describe --tags --exact-match --match 'v*' >/dev/null 2>&1 && echo 1)
ifneq ($(GIT_ON_TAG),1)
VERSION_DEFS := -DMIDGE_BUILD_HASH=\"$(GIT_HASH)\"
endif

# Containerised cross-build: same image as CI, so local m68k builds match.
DOCKER          ?= docker
AMIGA_GCC_IMAGE ?= ghcr.io/sidick/amiga-dev:1
# Run as the calling user, not root: the container bind-mounts $(CURDIR), and
# without this, files it creates come out root-owned on Linux hosts, breaking
# any later non-Docker step (e.g. `make dist`) that writes into build/.
DOCKER_USER     := --user "$(shell id -u):$(shell id -g)"

CORE_SRCS  := $(wildcard src/core/*.c)
TOOLS_SRCS := $(wildcard src/tools/*.c)
HOST_SRCS  := src/host/transport_bsd.c $(HOST_TLS_SRCS) src/host/args.c src/host/clock.c
AMIGA_SRCS := src/amiga/transport_bsdsocket.c src/amiga/args.c src/amiga/clock.c
LIB_SRCS   := $(wildcard src/library/*.c)
TEST_SRCS  := $(wildcard tests/test_*.c)

# Header dependencies for the whole-program link rules below. These compile
# all sources in one compiler invocation (no per-object .d files), so headers
# must be listed as prerequisites or edits to them rebuild nothing. A coarse
# wildcard is correct here - a header touch rebuilds in seconds.
CORE_HDRS  := $(wildcard src/core/*.h src/*.h)
TOOLS_HDRS := $(wildcard src/tools/*.h)
HOST_HDRS  := $(wildcard src/host/*.h)
AMIGA_HDRS := $(wildcard src/amiga/*.h)
LIB_HDRS   := $(wildcard src/library/*.h)
TEST_HDRS  := $(wildcard tests/*.h)

BUILD := build

# --- mqtt.library: sfdc-generated headers/stubs (Phase 2 slice 1) ---
# sfdc is part of the amiga-gcc toolchain (not built from source here); its
# .sfd source of truth lives in src/library/. Generated output is never
# checked in - it lands under build/, which is already gitignored.
SFDC        ?= sfdc
SFD_TARGET  := m68k-amigaos
LIB_SFD     := src/library/mqtt_lib.sfd
LIB_INCDIR  := $(BUILD)/include
LIB_GENDIR  := $(BUILD)/library-gen

.PHONY: all test cli broker-smoke broker-tls-smoke m68k m68k-docker codec-selftest-m68k codec-selftest-m68k-docker net-smoke volamos-smoke volamos-test-target guide api-reference dist clean build test-host test-target lint library-headers library libsmoke-m68k library-smoke volamos-library-smoke libnet-m68k library-net-smoke volamos-library-net-smoke libreconn-m68k library-reconnect-smoke examples example-smoke

all: test cli

# --- Verb contract (sidick/amiga-workflows' build-test.yml) ---------------
# ci.yml calls these five names; each build-test.yml job is independent (no
# artifact-passing between them). The named targets below (test/cli/m68k/...)
# stay as the documented local entry points.
build: m68k library examples

test-host: test cli

# AMIGA_REAL_ROM, if the workflow decoded one from the optional
# AMIGA_REAL_ROM_B64 secret, becomes KICK=; unset (the common case), boots
# Copperline's bundled AROS ROM instead.
test-target: codec-selftest-m68k
	KICK="$${AMIGA_REAL_ROM:-}" sh tests/copperline/run.sh

# No structural checks yet beyond the compiler; hold the core to -Werror so
# the strictest warning set gates merges even when built on a plain runner.
lint:
	$(CC) $(CFLAGS) -Werror $(CObjINC) -fsyntax-only $(CORE_SRCS)

# --- Host: unit tests ---
test: $(BUILD)/run-tests
	$(BUILD)/run-tests

$(BUILD)/run-tests: $(CORE_SRCS) $(TEST_SRCS) $(CORE_HDRS) $(TEST_HDRS) | $(BUILD)/.dir
	$(CC) $(CFLAGS) $(CObjINC) -Itests $(CORE_SRCS) $(TEST_SRCS) -o $@

# --- Host: native tools (for local development and the broker smoke test) ---
# Named distinctly from the m68k binaries so the two don't collide on a
# case-insensitive filesystem (macOS).
cli: $(BUILD)/mqtt_pub-host $(BUILD)/mqtt_sub-host

$(BUILD)/mqtt_pub-host: $(CORE_SRCS) $(TOOLS_SRCS) $(HOST_SRCS) src/host/pub_main.c $(CORE_HDRS) $(TOOLS_HDRS) $(HOST_HDRS) | $(BUILD)/.dir
	$(CC) $(CFLAGS) $(CObjINC) -Isrc/tools -Isrc/host $(HOST_TLS_CFLAGS) $(CORE_SRCS) $(TOOLS_SRCS) $(HOST_SRCS) src/host/pub_main.c -o $@ $(HOST_TLS_LIBS)

$(BUILD)/mqtt_sub-host: $(CORE_SRCS) $(TOOLS_SRCS) $(HOST_SRCS) src/host/sub_main.c $(CORE_HDRS) $(TOOLS_HDRS) $(HOST_HDRS) | $(BUILD)/.dir
	$(CC) $(CFLAGS) $(CObjINC) -Isrc/tools -Isrc/host $(HOST_TLS_CFLAGS) $(CORE_SRCS) $(TOOLS_SRCS) $(HOST_SRCS) src/host/sub_main.c -o $@ $(HOST_TLS_LIBS)

# --- Host: end-to-end smoke test against a local Mosquitto ---
broker-smoke: cli
	MQTT_PUB=$(BUILD)/mqtt_pub-host MQTT_SUB=$(BUILD)/mqtt_sub-host \
		sh tests/broker/smoke.sh

broker-tls-smoke: cli
	MQTT_PUB=$(BUILD)/mqtt_pub-host MQTT_SUB=$(BUILD)/mqtt_sub-host \
		sh tests/broker/tls-smoke.sh

# --- m68k: Amiga binaries (amiga-gcc on PATH) ---
# Four binaries: mqtt_pub/mqtt_sub are the default, library-linked tools
# (OpenLibrary("mqtt.library") + the MQTT_* API, see src/amiga/pub_main_lib.c
# / sub_main_lib.c) - they link ONLY src/amiga/args.c plus the *_main_lib.c
# itself (reusing the same ReadArgs template/tool_opts as the static build),
# never src/core or src/tools, so they need library-headers' caller-side
# headers (-I$(LIB_INCDIR)) but nothing from the mqtt.library build itself.
# mqtt_pub-static/mqtt_sub-static are exactly today's link line, renamed -
# the zero-install fallback that needs only bsdsocket.library.
m68k: library-headers | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) $(VERSION_DEFS) -I$(LIB_INCDIR) src/amiga/args.c src/amiga/pub_main_lib.c -o $(BUILD)/mqtt_pub
	$(M68K_CC) $(M68K_CFLAGS) $(VERSION_DEFS) -I$(LIB_INCDIR) src/amiga/args.c src/amiga/sub_main_lib.c -o $(BUILD)/mqtt_sub
	$(M68K_CC) $(M68K_CFLAGS) $(VERSION_DEFS) $(CORE_SRCS) $(TOOLS_SRCS) $(AMIGA_SRCS) src/amiga/pub_main.c -o $(BUILD)/mqtt_pub-static
	$(M68K_CC) $(M68K_CFLAGS) $(VERSION_DEFS) $(CORE_SRCS) $(TOOLS_SRCS) $(AMIGA_SRCS) src/amiga/sub_main.c -o $(BUILD)/mqtt_sub-static

m68k-docker:
	$(DOCKER) run --rm --platform linux/amd64 $(DOCKER_USER) -v "$(CURDIR)":/work -w /work \
		$(AMIGA_GCC_IMAGE) sh -lc 'PATH=/opt/amiga/bin:$$PATH make m68k GIT_HASH=$(GIT_HASH) GIT_ON_TAG=$(GIT_ON_TAG)'

# --- mqtt.library: sfdc-generated headers/stubs ---
# sfdc's `-o`/`--output` silently falls back to stdout instead of writing a
# file whenever the .sfd has zero public+private functions (verified
# against sfdc 1.11f - it never emits its usual "Writing to '...'" line in
# that case either, so this isn't a transient failure to paper over). Slice
# 1's mqtt_lib.sfd is intentionally empty (no library functions yet), so
# every rule below redirects stdout instead of passing -o. Revisit once
# slice 2 adds real functions - at that point -o should start working and
# either form is fine.
library-headers: $(LIB_INCDIR)/proto/mqtt.h $(LIB_INCDIR)/inline/mqtt.h \
	$(LIB_INCDIR)/clib/mqtt_protos.h \
	$(LIB_INCDIR)/fd/mqtt_lib.fd $(LIB_INCDIR)/libraries/mqtt.h \
	$(LIB_GENDIR)/gatestubs.c $(LIB_GENDIR)/functable.i $(LIB_GENDIR)/libproto.h

# proto/mqtt.h #includes <clib/mqtt_protos.h> (the plain, non-libbase-first
# prototypes) unconditionally - needed even though callers only ever go
# through the inline/mqtt.h macros (GNUC path), since that #include line
# itself must resolve.
$(LIB_INCDIR)/clib/mqtt_protos.h: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_INCDIR)/clib
	$(SFDC) --mode=clib --target=$(SFD_TARGET) $(LIB_SFD) > $@

# The public structs header is hand-written (src/library/include/), not
# sfdc-generated - callers reach it as <libraries/mqtt.h> via this same
# -Ibuild/include as the sfdc-generated proto/inline/fd headers.
$(LIB_INCDIR)/libraries/mqtt.h: src/library/include/libraries/mqtt.h | $(BUILD)/.dir
	@mkdir -p $(LIB_INCDIR)/libraries
	cp $< $@

$(LIB_INCDIR)/proto/mqtt.h: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_INCDIR)/proto
	$(SFDC) --mode=proto --target=$(SFD_TARGET) $(LIB_SFD) > $@

$(LIB_INCDIR)/inline/mqtt.h: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_INCDIR)/inline
	$(SFDC) --mode=macros --target=$(SFD_TARGET) $(LIB_SFD) > $@

$(LIB_INCDIR)/fd/mqtt_lib.fd: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_INCDIR)/fd
	$(SFDC) --mode=fd $(LIB_SFD) > $@

# Library-side gate stubs (the trampolines library functions are called
# through), the function table they populate, and the libbase-first
# prototypes header - all three feed the `library` link below, not
# consumed by mqtt.library's callers.
#
# --gateprefix=Gate_: without it, sfdc's gatestubs mode emits the register-
# parameter trampoline under the SAME name as the plain C function it
# calls (e.g. two conflicting `MQTT_CreateClient`s) - verified against
# sfdc 1.11f/m68k-amigaos-gcc: that fails to compile ("conflicting types").
# The prefixed trampoline is what libinit.c's ADD2LIST() calls reference;
# mqtt_funcs.c implements the plain (unprefixed) names the trampolines
# call into.
$(LIB_GENDIR)/gatestubs.c: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_GENDIR)
	$(SFDC) --mode=gatestubs --target=$(SFD_TARGET) --libarg=first --gateprefix=Gate_ $(LIB_SFD) > $@

$(LIB_GENDIR)/functable.i: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_GENDIR)
	$(SFDC) --mode=functable --target=$(SFD_TARGET) $(LIB_SFD) > $@

# Prototypes for the plain (unprefixed) library functions, base-first -
# included by mqtt_funcs.c purely so the compiler checks its function
# definitions against the exact signatures sfdc derived from the .sfd
# (catches a register/type mismatch at compile time instead of leaving it
# as a silent cross-TU link-time footgun against gatestubs.c).
$(LIB_GENDIR)/libproto.h: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_GENDIR)
	$(SFDC) --mode=libproto --target=$(SFD_TARGET) --libarg=first $(LIB_SFD) > $@

# --- mqtt.library: link ---
# libinit.o (libnix's single-data-segment shared-library startup - see the
# "libnix" skill) replaces the normal C startup, so -nostartfiles keeps the
# gcc driver from also pulling in crt0/libnix's program startup. No
# -fbaserel: this is libinit.o, not the per-caller-datasegment libinitr.o.
# src/core is linked in unmodified (portable C99, no OS deps - CLAUDE.md).
# transport_bsdsocket.c/clock.c are the same src/amiga platform files the
# CLI tools use (NOT args.c/pub_main.c/sub_main.c, which are CLI-only) -
# mqtt_funcs.c's per-connection subprocess drives mqtt_client through them
# exactly like src/amiga/pub_main.c/sub_main.c do.
# -Isrc/library/include reaches the hand-written <libraries/mqtt.h> (both
# gatestubs.c and mqtt_funcs.c include it); -I$(LIB_GENDIR) reaches the
# sfdc-generated libproto.h mqtt_funcs.c checks its own signatures against.
LIBINIT_O := /opt/amiga/m68k-amigaos/libnix/lib/libinit.o

library: $(BUILD)/mqtt.library

# AMISSL_SRCS/_CFLAGS/_LIBS are empty unless `make fetch-amissl-sdk` has
# populated AMISSL_SDK_DIR (see the feature-detect comment above) - the
# library still links fine without them, just without mco_TLS support
# (transport_amissl_connect() unreferenced, so nothing pulls it in).
$(BUILD)/mqtt.library: $(LIB_SRCS) $(CORE_SRCS) src/amiga/transport_bsdsocket.c src/amiga/clock.c \
		$(AMISSL_SRCS) \
		$(LIB_HDRS) $(CORE_HDRS) $(AMIGA_HDRS) \
		$(LIB_GENDIR)/gatestubs.c $(LIB_GENDIR)/libproto.h | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) $(VERSION_DEFS) $(AMISSL_CFLAGS) -nostartfiles \
		-Isrc/library/include -I$(LIB_GENDIR) \
		$(LIBINIT_O) $(LIB_SRCS) $(CORE_SRCS) \
		src/amiga/transport_bsdsocket.c src/amiga/clock.c $(AMISSL_SRCS) \
		$(LIB_GENDIR)/gatestubs.c \
		$(AMISSL_LIBS) \
		-o $@

# --- mqtt.library: example caller programs (developer-facing, shipped in
# dist's midge/developer/) ---
# Built exactly as a third-party mqtt.library caller would: -I$(LIB_INCDIR)
# reaches ONLY <libraries/mqtt.h>/<proto/mqtt.h>/<inline/mqtt.h> (the
# sfdc-generated caller-side headers, see library-headers above) - the
# examples' C sources never #include anything from src/. -Isrc is added
# only so they can pull in src/version.h for their $VER string
# (MIDGE_VERSTAG, see src/amiga/pub_main.c) - a build-time convenience, not
# an API dependency; M68K_CFLAGS's other -Isrc* entries are harmless no-ops
# here since the sources never reference anything under them.
examples: library-headers $(BUILD)/pubexample $(BUILD)/subexample

$(BUILD)/pubexample: examples/pubexample.c src/version.h $(LIB_INCDIR)/libraries/mqtt.h | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) -I$(LIB_INCDIR) examples/pubexample.c -o $@

$(BUILD)/subexample: examples/subexample.c src/version.h $(LIB_INCDIR)/libraries/mqtt.h | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) -I$(LIB_INCDIR) examples/subexample.c -o $@

# --- m68k: on-target codec self-test (run by test-target via Copperline) ---
codec-selftest-m68k: | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) -Itests $(CORE_SRCS) tests/copperline/codec_selftest.c -o $(BUILD)/codec_selftest

codec-selftest-m68k-docker:
	$(DOCKER) run --rm --platform linux/amd64 $(DOCKER_USER) -v "$(CURDIR)":/work -w /work \
		$(AMIGA_GCC_IMAGE) sh -lc 'PATH=/opt/amiga/bin:$$PATH make codec-selftest-m68k'

# --- Same codec check via volamos: no emulated boot, much faster local loop ---
# Not a CI/release gate (see tests/copperline/volamos-run.sh) - just a
# quicker way to iterate on the codec than a full Copperline boot.
volamos-test-target: codec-selftest-m68k
	sh tests/copperline/volamos-run.sh

# --- m68k: on-target mqtt.library OpenLibrary/CloseLibrary smoke test ---
libsmoke-m68k: | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) tests/library/libsmoke.c -o $(BUILD)/libsmoke

# Real Copperline boot (CI/release gate): stages build/libsmoke and
# build/mqtt.library into a throwaway boot volume and asserts the serial
# capture - see tests/library/run.sh.
library-smoke: library libsmoke-m68k
	sh tests/library/run.sh

# --- Same check via volamos: no emulated boot, much faster local loop ---
# Not a CI/release gate (see tests/library/volamos-run.sh) - just a quicker
# way to iterate on mqtt.library than a full Copperline boot.
volamos-library-smoke: library libsmoke-m68k
	sh tests/library/volamos-run.sh

# --- m68k: on-target mqtt.library end-to-end network test harness ---
# A normal libnix CLI program (not the library skeleton) built against the
# generated caller-side headers (proto/mqtt.h + inline/mqtt.h), exactly as
# any other mqtt.library client would be - see tests/library/libnet.c.
libnet-m68k: library-headers | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) -I$(LIB_INCDIR) tests/library/libnet.c -o $(BUILD)/libnet

# Real Copperline boot (CI/release gate): stages build/libnet and
# build/mqtt.library into a throwaway boot volume with Copperline's
# HostSocket board fitted, runs it against a real scratch Mosquitto, and
# asserts both the serial capture and a host observer - see
# tests/library/net-run.sh.
library-net-smoke: library libnet-m68k cli
	sh tests/library/net-run.sh

# --- Same check via volamos: no emulated boot, much faster local loop ---
# Not a CI/release gate (see tests/library/volamos-net-run.sh) - just a
# quicker way to iterate than a full Copperline boot. May SKIP (exit 0) if
# volamos doesn't support mqtt.library's CreateNewProcTags/MsgPort
# subprocess model - see that script's banner.
volamos-library-net-smoke: library libnet-m68k cli
	sh tests/library/volamos-net-run.sh

# --- m68k: on-target mco_AutoReconnect test harness ---
# Same shape as libnet-m68k (a normal CLI program, not the library skeleton
# itself) - see tests/library/libreconn.c.
libreconn-m68k: library-headers | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) -I$(LIB_INCDIR) tests/library/libreconn.c -o $(BUILD)/libreconn

# Real Copperline boot (CI/release gate): stages build/libreconn and
# build/mqtt.library into a throwaway boot volume with Copperline's
# HostSocket board fitted, runs it against a real scratch Mosquitto that
# this test kills and restarts mid-run (same port) to prove
# mco_AutoReconnect's backoff + auto-resubscribe actually recovers the
# session - see tests/library/reconn-run.sh. Needs mqtt_pub-host (`make
# cli`) to retained-publish the phase1/phase2 messages, same as
# library-net-smoke.
library-reconnect-smoke: library libreconn-m68k cli
	sh tests/library/reconn-run.sh

# --- m68k: on-target mco_TLS end-to-end smoke test (issue #3 Phase 3) ---
# Same shape as libnet-m68k (a normal CLI program against the generated
# caller-side headers only - no AmiSSL headers needed by the caller) - see
# tests/library/libtls.c.
libtls-m68k: library-headers | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) -I$(LIB_INCDIR) tests/library/libtls.c -o $(BUILD)/libtls

# Local-only, NOT a CI/release gate (unlike library-net-smoke): needs a
# real AmigaOS 3.2.2 + AmiSSL boot image built by amibake
# (github.com/sidick/amibake), since AmiSSL needs real Devs:/Libs: install
# layout that the plain bundled-AROS/HostSocket boot the other
# library-*-smoke targets use doesn't have - see
# tests/library/README.md's "TLS smoke test" section for setup. Also
# needs `library` built with AmiSSL support (`make fetch-amissl-sdk`
# first - see M68K_HAS_AMISSL above), and $MIDGE_TLS_AMIGA_IMAGE pointing
# at the amibake output directory.
library-tls-smoke: library libtls-m68k cli
	sh tests/library/tls-run.sh

# --- m68k: on-target mco_CAFile end-to-end smoke test (issue #13) ---
# Same shape/local-only rationale as libtls-m68k/library-tls-smoke - see
# tests/library/libcafile.c and cafile-run.sh's own banners (the latter
# also explains why this one seeds Copperline's RTC).
libcafile-m68k: library-headers | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) -I$(LIB_INCDIR) tests/library/libcafile.c -o $(BUILD)/libcafile

library-cafile-smoke: library libcafile-m68k
	sh tests/library/cafile-run.sh

# --- m68k: on-target check that examples/pubexample.c actually works ---
# Real Copperline boot (CI/release gate): stages build/pubexample and
# build/mqtt.library into a throwaway boot volume with Copperline's
# HostSocket board fitted, runs it against a real scratch Mosquitto with a
# host mqtt_sub-host observer, and asserts the observer saw the publish -
# see tests/library/example-run.sh.
example-smoke: library examples cli
	sh tests/library/example-run.sh

# --- On-target network smoke: Copperline HostSocket -> host Mosquitto ---
# No machine-specific assets needed - see tests/net/README.md. Needs
# `library` too: net-smoke.sh stages build/mqtt.library for the default,
# library-linked mqtt_pub build it now also exercises alongside
# mqtt_pub-static.
net-smoke: m68k library cli
	sh tests/net/net-smoke.sh

# --- Same check via volamos: no emulated boot, much faster local loop ---
# Not a CI/release gate (see tests/net/volamos-smoke.sh) - just a quicker
# way to iterate on the Amiga networking code than a full Copperline boot.
# Only exercises mqtt_pub-static (volamos can't run mqtt.library's
# subprocess-per-client model) - no `library` dependency needed here.
volamos-smoke: m68k cli
	sh tests/net/volamos-smoke.sh

# --- api-reference: userdocs/mqtt-library-reference.md, generated from ------
# src/library/mqtt.doc (the API reference autodoc). Checked in like any
# other userdocs page (mkdocs' docs_dir needs it physically present, and
# the shared docs-deploy workflow has no hook to generate it first) - but
# it's generated, not hand-edited: edit mqtt.doc, then rerun this.
api-reference:
	python3 tools/autodoc2md.py src/library/mqtt.doc userdocs/mqtt-library-reference.md

# --- guide: AmigaGuide user documentation, generated from userdocs/ ----------
guide: | $(BUILD)/.dir
	python3 tools/docs2guide.py userdocs $(BUILD)/midge.guide

# --- lha: build the real LHa for UNIX (archive-capable), pinned --------------
# Homebrew's and Ubuntu's `lha` is Lhasa - extract-only, useless for packaging
# - and the last lha *release* tag (2021) no longer compiles with modern
# compilers, so build a pinned master commit from source into build/tools/.
# Needs git + autoconf/automake. Override with a known-good archiver:
#   make dist LHA=/path/to/real/lha
LHA_REPO   := https://github.com/jca02266/lha.git
LHA_COMMIT := 86094cb56aba34de45668f39f74fcfb61e9d7fb6
LHA        ?= $(BUILD)/tools/lha

$(BUILD)/tools/lha:
	@mkdir -p $(BUILD)/tools
	rm -rf $(BUILD)/tools/lha-src
	git clone -q $(LHA_REPO) $(BUILD)/tools/lha-src
	cd $(BUILD)/tools/lha-src && \
		git -c advice.detachedHead=false checkout -q $(LHA_COMMIT) && \
		autoreconf -fi >/dev/null 2>&1 && ./configure >/dev/null && \
		$(MAKE) >/dev/null
	cp $(BUILD)/tools/lha-src/src/lha $(BUILD)/tools/lha
	rm -rf $(BUILD)/tools/lha-src

# --- dist: assemble the Aminet upload pair (archive + .readme) ---------------
# Builds the m68k binaries itself (build-test.yml's dist job runs `make dist`
# standalone, with no prior `build` job's artifacts to reuse). The $VER grep
# confirms the binaries just built embed the CURRENT src/version.h
# MIDGE_VERSION; the release workflow's tag-vs-source check
# (scripts/verify-version.sh) separately confirms the tag matches
# src/version.h, closing the loop: tag == src/version.h == the binaries.
#
# Unlike `build`/`library`/`test-host` (which feature-detect AmiSSL and
# degrade gracefully - the shared CI image has no SDK, see M68K_HAS_AMISSL
# above), a release MUST ship a TLS-capable mqtt.library: the readme/docs
# advertise TLS unconditionally. So `dist` fetches the SDK itself if
# missing and re-invokes `build` as a sub-make ($(MAKE), not a plain
# prerequisite) - M68K_HAS_AMISSL is a `:=` immediate-expansion variable,
# fixed when the OUTER make parses this file, before any recipe (including
# the fetch below) has run; only a fresh sub-make process re-parses the
# Makefile and sees the now-populated .cache/amissl-sdk/. The `rm -f`
# before it is required too - `make` has no idea M68K_HAS_AMISSL changed,
# only that mqtt.library's sources didn't, and would otherwise report
# "Nothing to be done" and reuse the stale non-TLS build. The grep after
# is a mechanical, permanent guard against this whole class of regression
# (a silently-degraded release), not just a one-time fix.
dist: guide $(LHA)
	@if [ ! -f "$(AMISSL_SDK_DIR)/include/openssl/ssl.h" ]; then \
		echo "dist: fetching AmiSSL SDK (a release must ship TLS support)"; \
		$(MAKE) fetch-amissl-sdk; \
	fi
	rm -f $(BUILD)/mqtt.library
	$(MAKE) build
	@grep -aq "amisslmaster.library" $(BUILD)/mqtt.library || { echo "dist: $(BUILD)/mqtt.library has no AmiSSL/TLS support - refusing to release (see M68K_HAS_AMISSL / make fetch-amissl-sdk)"; exit 1; }
	@v=$$(sed -n 's/^#define MIDGE_VERSION[[:space:]]*"\(.*\)"$$/\1/p' src/version.h); \
	for b in mqtt_pub mqtt_sub mqtt_pub-static mqtt_sub-static; do \
		grep -aqF "\$$VER: $$b $$v (" $(BUILD)/$$b || { echo "dist: $(BUILD)/$$b lacks \"\$$VER: $$b $$v (...)\" - stale build/?"; exit 1; }; \
	done; \
	grep -aqF "\$$VER: mqtt.library 1.0 (" $(BUILD)/mqtt.library || { echo "dist: $(BUILD)/mqtt.library lacks \"\$$VER: mqtt.library 1.0 (...)\" - stale build/? (library \$$VER tracks LibVersion.LibRevision, not MIDGE_VERSION - see src/library/libinit.c)"; exit 1; }
	rm -rf $(BUILD)/dist
	mkdir -p $(BUILD)/dist/midge/libs $(BUILD)/dist/midge/developer/fd \
		$(BUILD)/dist/midge/developer/clib $(BUILD)/dist/midge/developer/proto \
		$(BUILD)/dist/midge/developer/inline $(BUILD)/dist/midge/developer/libraries \
		$(BUILD)/dist/midge/developer/examples
	cp $(BUILD)/mqtt_pub $(BUILD)/mqtt_sub $(BUILD)/mqtt_pub-static $(BUILD)/mqtt_sub-static \
		$(BUILD)/midge.guide LICENSE midge.readme $(BUILD)/dist/midge/
	cp $(BUILD)/mqtt.library $(BUILD)/dist/midge/libs/
	cp src/library/mqtt_lib.sfd src/library/mqtt.doc $(BUILD)/dist/midge/developer/
	cp $(LIB_INCDIR)/fd/mqtt_lib.fd $(BUILD)/dist/midge/developer/fd/
	cp $(LIB_INCDIR)/clib/mqtt_protos.h $(BUILD)/dist/midge/developer/clib/
	cp $(LIB_INCDIR)/proto/mqtt.h $(BUILD)/dist/midge/developer/proto/
	cp $(LIB_INCDIR)/inline/mqtt.h $(BUILD)/dist/midge/developer/inline/
	cp $(LIB_INCDIR)/libraries/mqtt.h $(BUILD)/dist/midge/developer/libraries/
	cp examples/pubexample.c examples/subexample.c $(BUILD)/dist/midge/developer/examples/
	cp midge.readme $(BUILD)/dist/
	cd $(BUILD)/dist && $(abspath $(LHA)) aq midge.lha midge
	@ls -l $(BUILD)/dist/midge.lha $(BUILD)/dist/midge.readme

# Order-only mkdir sentinel. Deliberately NOT a target named `build` - that
# would merge prerequisites with the verb-contract `build:` target above,
# silently giving every `| $(BUILD)/.dir` rule an m68k dependency and
# breaking `make test` on hosts with no cross-compiler.
$(BUILD)/.dir:
	@mkdir -p $(BUILD)
	@touch $@

clean:
	rm -rf $(BUILD)
