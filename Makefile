# midge build.
#
#   make test        build and run the host-side unit tests (default)
#   make cli         build the host-native mqtt_pub/mqtt_sub tools
#   make broker-smoke  run the host tools against a local Mosquitto
#   make m68k        cross-build the Amiga binaries (needs amiga-gcc on PATH)
#   make m68k-docker cross-build inside the CI container (no local toolchain)
#   make net-smoke   on-target network test (Copperline HostSocket; no ROM/WB needed)
#   make volamos-smoke  same check via volamos - faster local loop, not a CI substitute
#   make volamos-test-target  codec self-test via volamos - faster local loop
#   make library-smoke  on-target mqtt.library OpenLibrary/CloseLibrary smoke test
#   make volamos-library-smoke  same check via volamos - faster local loop
#   make clean
#
# The core is portable C99, so `test` and `cli` build with any host compiler.

# --- Host toolchain (tests + native tools) ---
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
CObjINC := -Isrc/core

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
GIT_ON_TAG ?= $(shell git describe --tags --exact-match >/dev/null 2>&1 && echo 1)
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
HOST_SRCS  := src/host/transport_bsd.c src/host/args.c src/host/clock.c
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

.PHONY: all test cli broker-smoke m68k m68k-docker codec-selftest-m68k codec-selftest-m68k-docker net-smoke volamos-smoke volamos-test-target guide dist clean build test-host test-target lint library-headers library libsmoke-m68k library-smoke volamos-library-smoke

all: test cli

# --- Verb contract (sidick/amiga-workflows' build-test.yml) ---------------
# ci.yml calls these five names; each build-test.yml job is independent (no
# artifact-passing between them). The named targets below (test/cli/m68k/...)
# stay as the documented local entry points.
build: m68k

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
	$(CC) $(CFLAGS) $(CObjINC) -Isrc/tools -Isrc/host $(CORE_SRCS) $(TOOLS_SRCS) $(HOST_SRCS) src/host/pub_main.c -o $@

$(BUILD)/mqtt_sub-host: $(CORE_SRCS) $(TOOLS_SRCS) $(HOST_SRCS) src/host/sub_main.c $(CORE_HDRS) $(TOOLS_HDRS) $(HOST_HDRS) | $(BUILD)/.dir
	$(CC) $(CFLAGS) $(CObjINC) -Isrc/tools -Isrc/host $(CORE_SRCS) $(TOOLS_SRCS) $(HOST_SRCS) src/host/sub_main.c -o $@

# --- Host: end-to-end smoke test against a local Mosquitto ---
broker-smoke: cli
	MQTT_PUB=$(BUILD)/mqtt_pub-host MQTT_SUB=$(BUILD)/mqtt_sub-host \
		sh tests/broker/smoke.sh

# --- m68k: Amiga binaries (amiga-gcc on PATH) ---
m68k: | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) $(VERSION_DEFS) $(CORE_SRCS) $(TOOLS_SRCS) $(AMIGA_SRCS) src/amiga/pub_main.c -o $(BUILD)/mqtt_pub
	$(M68K_CC) $(M68K_CFLAGS) $(VERSION_DEFS) $(CORE_SRCS) $(TOOLS_SRCS) $(AMIGA_SRCS) src/amiga/sub_main.c -o $(BUILD)/mqtt_sub

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
	$(LIB_INCDIR)/fd/mqtt_lib.fd $(LIB_GENDIR)/gatestubs.c $(LIB_GENDIR)/functable.i

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
# through) and the function table they populate - both feed the `library`
# link below, not consumed by mqtt.library's callers.
$(LIB_GENDIR)/gatestubs.c: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_GENDIR)
	$(SFDC) --mode=gatestubs --target=$(SFD_TARGET) --libarg=first $(LIB_SFD) > $@

$(LIB_GENDIR)/functable.i: $(LIB_SFD) | $(BUILD)/.dir
	@mkdir -p $(LIB_GENDIR)
	$(SFDC) --mode=functable --target=$(SFD_TARGET) $(LIB_SFD) > $@

# --- mqtt.library: link ---
# libinit.o (libnix's single-data-segment shared-library startup - see the
# "libnix" skill) replaces the normal C startup, so -nostartfiles keeps the
# gcc driver from also pulling in crt0/libnix's program startup. No
# -fbaserel: this is libinit.o, not the per-caller-datasegment libinitr.o.
# src/core is linked in already (portable C99, no OS deps) so the layout
# is ready for slice 2 - nothing calls into it yet since mqtt_lib.sfd has
# no functions.
LIBINIT_O := /opt/amiga/m68k-amigaos/libnix/lib/libinit.o

library: $(BUILD)/mqtt.library

$(BUILD)/mqtt.library: $(LIB_SRCS) $(CORE_SRCS) $(LIB_HDRS) $(CORE_HDRS) $(LIB_GENDIR)/gatestubs.c | $(BUILD)/.dir
	$(M68K_CC) $(M68K_CFLAGS) $(VERSION_DEFS) -nostartfiles \
		$(LIBINIT_O) $(LIB_SRCS) $(CORE_SRCS) $(LIB_GENDIR)/gatestubs.c \
		-o $@

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

# --- On-target network smoke: Copperline HostSocket -> host Mosquitto ---
# No machine-specific assets needed - see tests/net/README.md.
net-smoke: m68k cli
	sh tests/net/net-smoke.sh

# --- Same check via volamos: no emulated boot, much faster local loop ---
# Not a CI/release gate (see tests/net/volamos-smoke.sh) - just a quicker
# way to iterate on the Amiga networking code than a full Copperline boot.
volamos-smoke: m68k cli
	sh tests/net/volamos-smoke.sh

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
dist: build guide $(LHA)
	@v=$$(sed -n 's/^#define MIDGE_VERSION[[:space:]]*"\(.*\)"$$/\1/p' src/version.h); \
	for b in mqtt_pub mqtt_sub; do \
		grep -aqF "\$$VER: $$b $$v (" $(BUILD)/$$b || { echo "dist: $(BUILD)/$$b lacks \"\$$VER: $$b $$v (...)\" - stale build/?"; exit 1; }; \
	done
	rm -rf $(BUILD)/dist
	mkdir -p $(BUILD)/dist/midge
	cp $(BUILD)/mqtt_pub $(BUILD)/mqtt_sub $(BUILD)/midge.guide \
		LICENSE midge.readme $(BUILD)/dist/midge/
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
