# SPDX-License-Identifier: GPL-2.0-or-later
# Host build and tests. The OpenWrt packages build from openwrt/ (see
# openwrt/README.md), with CC, CFLAGS, LDFLAGS and VERSION from the SDK.

CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD   := build
# The release, for --version: PKG_VERSION of the OpenWrt package.
VERSION ?= $(shell sed -n 's/^PKG_VERSION:=//p' openwrt/net/opendectcell/Makefile)
DEFS    := -DOPENDECTCELL_VERSION='"$(VERSION)"'

# Local AVM files for the optional real-file tests (never in git).
FW7520  := work/7520/rootfs/lib/firmware
export OPENDECTCELL_STUB_HEX ?= $(FW7520)/dectfw_secondlevel_441.hex
export OPENDECTCELL_FL_HEX   ?= $(FW7520)/dectfw_firstlevel_441_slow.hex

TESTS := $(BUILD)/test_fwimage $(BUILD)/test_mail $(BUILD)/test_session $(BUILD)/test_audio $(BUILD)/test_cs $(BUILD)/test_lc $(BUILD)/test_lapc $(BUILD)/test_nwk $(BUILD)/test_dsaa $(BUILD)/test_mm $(BUILD)/test_cc $(BUILD)/test_iwu $(BUILD)/test_g722 $(BUILD)/test_clip $(BUILD)/test_callinfo $(BUILD)/test_time $(BUILD)/test_link $(BUILD)/test_diag $(BUILD)/test_profile $(BUILD)/test_ctl $(BUILD)/test_ssc_align $(BUILD)/test_cell $(BUILD)/test_trace $(BUILD)/test_contacts $(BUILD)/test_calls $(BUILD)/test_stubd $(BUILD)/test_stubd_cell $(BUILD)/test_base $(BUILD)/test_peers
FWLOAD_SRC := src/fwload/dect-fwload.c src/fwload/loader.c src/fwload/fwimage.c

# Cross build for the 7520 with the OpenWrt SDK (the release of tools/release/OPENWRT_VERSION, ipq40xx/generic).
SDK     ?= $(firstword $(wildcard $(HOME)/opt/openwrt-sdk-$(shell cat tools/release/OPENWRT_VERSION)-ipq40xx-*))
TCBIN   := $(firstword $(wildcard $(SDK)/staging_dir/toolchain-*/bin))
XCC     := $(TCBIN)/arm-openwrt-linux-gcc

# dect-stubd: framing, session, the C-plane stack, and the A-law codec for the
# transmit test tone (dect_audio.c pulls in libm).
STUBD_SRC := src/stubd/dect-stubd.c src/stubd/ssc_align.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_session.c \
	src/libopendectcell/dect_cs.c src/libopendectcell/dect_lc.c src/libopendectcell/dect_lapc.c \
	src/libopendectcell/dect_nwk.c src/libopendectcell/dect_dsaa.c src/libopendectcell/dect_mm.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c \
	src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_audio.c \
	src/libopendectcell/dect_g722.c src/libopendectcell/dect_link.c src/libopendectcell/dect_time.c \
	src/libopendectcell/dect_diag.c src/libopendectcell/dect_profile.c src/libopendectcell/dect_ctl.c \
	src/libopendectcell/dect_cell.c src/libopendectcell/dect_base.c
STUBD_INC := -Ikernel/include/uapi -Isrc/libopendectcell

.PHONY: all check check-cell cross clean baresip-mod dect-cell
all: $(TESTS) $(BUILD)/dect-fwload $(BUILD)/dect-pcmcap $(BUILD)/dect-stubd $(BUILD)/dect-maildump

$(BUILD)/dect-fwload: $(FWLOAD_SRC) src/fwload/loader.h src/fwload/fwimage.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(LDFLAGS) -o $@ $(FWLOAD_SRC)

# Host build of dect-pcmcap for offline analysis (-S FILE) of captures.
$(BUILD)/dect-pcmcap: src/tools/dect-pcmcap.c src/libopendectcell/dect_audio.c src/libopendectcell/dect_audio.h kernel/include/uapi/opendectcell.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(LDFLAGS) -Ikernel/include/uapi -Isrc/libopendectcell -o $@ src/tools/dect-pcmcap.c src/libopendectcell/dect_audio.c -lm

# Offline: recover the full mail stream from a `dect-stubd -w` capture. The
# control slot is in every capture, so this works on runs already taken.
$(BUILD)/dect-maildump: src/tools/dect-maildump.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_mail.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc/libopendectcell -o $@ src/tools/dect-maildump.c src/libopendectcell/dect_mail.c

# Host build of dect-stubd: compiles everywhere, runs only on the box.
$(BUILD)/dect-stubd: $(STUBD_SRC) src/stubd/ssc_align.h src/libopendectcell/dect_mail.h src/libopendectcell/dect_session.h src/libopendectcell/dect_proto.h kernel/include/uapi/opendectcell.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(LDFLAGS) $(STUBD_INC) -o $@ $(STUBD_SRC) -lm

# For the 7520: build/dect-fwload.arm, dect-pcmcap.arm and dect-stubd.arm,
# stripped, ready to copy to the box (dect-fwload.arm.dbg keeps symbols).
cross: $(FWLOAD_SRC)
	@test -x "$(XCC)" || { echo "OpenWrt SDK not found: set SDK="; exit 1; }
	@mkdir -p $(BUILD)
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror -Wshadow $(DEFS) \
		-o $(BUILD)/dect-fwload.arm.dbg $(FWLOAD_SRC)
	$(TCBIN)/arm-openwrt-linux-muslgnueabi-strip -o $(BUILD)/dect-fwload.arm $(BUILD)/dect-fwload.arm.dbg
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror -Ikernel/include/uapi -Isrc/libopendectcell $(DEFS) \
		-o $(BUILD)/dect-pcmcap.arm src/tools/dect-pcmcap.c src/libopendectcell/dect_audio.c -lm
	$(TCBIN)/arm-openwrt-linux-muslgnueabi-strip $(BUILD)/dect-pcmcap.arm
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror $(STUBD_INC) $(DEFS) \
		-o $(BUILD)/dect-stubd.arm $(STUBD_SRC) -lm
	$(TCBIN)/arm-openwrt-linux-muslgnueabi-strip $(BUILD)/dect-stubd.arm

$(BUILD)/test_fwimage: tests/test_fwimage.c src/fwload/fwimage.c src/fwload/fwimage.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_fwimage.c src/fwload/fwimage.c

$(BUILD)/test_mail: tests/test_mail.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_mail.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_mail.c src/libopendectcell/dect_mail.c

$(BUILD)/test_session: tests/test_session.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_session.c src/libopendectcell/dect_session.h src/libopendectcell/dect_proto.h src/libopendectcell/dect_mail.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_session.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_session.c

$(BUILD)/test_audio: tests/test_audio.c src/libopendectcell/dect_audio.c src/libopendectcell/dect_audio.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_audio.c src/libopendectcell/dect_audio.c -lm

$(BUILD)/test_cs: tests/test_cs.c src/libopendectcell/dect_cs.c src/libopendectcell/dect_cs.h src/libopendectcell/dect_proto.h src/libopendectcell/dect_mail.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_cs.c src/libopendectcell/dect_cs.c

$(BUILD)/test_lc: tests/test_lc.c src/libopendectcell/dect_lc.c src/libopendectcell/dect_lc.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_lc.c src/libopendectcell/dect_lc.c

$(BUILD)/test_lapc: tests/test_lapc.c src/libopendectcell/dect_lapc.c src/libopendectcell/dect_lapc.h src/libopendectcell/dect_lc.c src/libopendectcell/dect_lc.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_lapc.c src/libopendectcell/dect_lapc.c src/libopendectcell/dect_lc.c

$(BUILD)/test_nwk: tests/test_nwk.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_nwk.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_nwk.c src/libopendectcell/dect_nwk.c

$(BUILD)/test_dsaa: tests/test_dsaa.c src/libopendectcell/dect_dsaa.c src/libopendectcell/dect_dsaa.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_dsaa.c src/libopendectcell/dect_dsaa.c

$(BUILD)/test_mm: tests/test_mm.c src/libopendectcell/dect_mm.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_dsaa.c src/libopendectcell/dect_mm.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_mm.c src/libopendectcell/dect_mm.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_dsaa.c

$(BUILD)/test_iwu: tests/test_iwu.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_iwu.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_iwu.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c

# Daemon-level golden traces: dect-stubd.c compiled in with STUBD_TEST
# (docs/stack-in-library.md, stage S0); its files go to build/stubd-test-run.
$(BUILD)/test_stubd: tests/test_stubd.c $(STUBD_SRC) src/stubd/ssc_align.h tests/check.h $(wildcard tests/stubd-traces/*.trace)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(SAN) $(STUBD_INC) -DSTUBD_TEST -DDECT_RUN_DIR='"$(BUILD)/stubd-test-run"' \
		-DAB_PATH='"$(BUILD)/stubd-test-run/audio.sock"' \
		-o $@ tests/test_stubd.c $(filter-out src/stubd/dect-stubd.c,$(STUBD_SRC)) -lm

# The cell side's media (dect-stubd -K, cell-protocol.md §8), the same way
$(BUILD)/test_stubd_cell: tests/test_stubd_cell.c $(STUBD_SRC) src/stubd/ssc_align.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(SAN) $(STUBD_INC) -DSTUBD_TEST -DDECT_RUN_DIR='"$(BUILD)/stubd-cell-run"' \
		-DAB_PATH='"$(BUILD)/stubd-cell-run/audio.sock"' \
		-o $@ tests/test_stubd_cell.c $(filter-out src/stubd/dect-stubd.c,$(STUBD_SRC)) -lm

$(BUILD)/test_base: tests/test_base.c $(filter src/libopendectcell/%,$(STUBD_SRC)) src/libopendectcell/dect_base.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -Isrc/libopendectcell -o $@ tests/test_base.c $(filter src/libopendectcell/%,$(STUBD_SRC)) -lm

$(BUILD)/test_calls: tests/test_calls.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_calls.h src/libopendectcell/dect_iwu.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_calls.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c

$(BUILD)/test_contacts: tests/test_contacts.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.h src/libopendectcell/dect_iwu.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_contacts.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c

$(BUILD)/test_cc: tests/test_cc.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_cc.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_cc.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c

$(BUILD)/test_clip: tests/test_clip.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_clip.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_clip.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_callinfo.c

$(BUILD)/test_time: tests/test_time.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_time.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_time.c src/libopendectcell/dect_time.c src/libopendectcell/dect_nwk.c

$(BUILD)/test_ssc_align: tests/test_ssc_align.c src/stubd/ssc_align.c src/stubd/ssc_align.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_ssc_align.c src/stubd/ssc_align.c

$(BUILD)/test_callinfo: tests/test_callinfo.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_callinfo.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_callinfo.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_nwk.c

# Conformance against ffmpeg is a separate, optional step: tools/g722-crosscheck.sh
$(BUILD)/test_g722: tests/test_g722.c src/libopendectcell/dect_g722.c src/libopendectcell/dect_g722.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_g722.c src/libopendectcell/dect_g722.c -lm

$(BUILD)/test_diag: tests/test_diag.c src/libopendectcell/dect_diag.c src/libopendectcell/dect_link.c src/libopendectcell/dect_time.c src/libopendectcell/dect_cs.c src/libopendectcell/dect_lc.c src/libopendectcell/dect_lapc.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_mm.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_dsaa.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_diag.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -Isrc/libopendectcell -o $@ tests/test_diag.c src/libopendectcell/dect_diag.c src/libopendectcell/dect_link.c src/libopendectcell/dect_time.c src/libopendectcell/dect_cs.c src/libopendectcell/dect_lc.c src/libopendectcell/dect_lapc.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_mm.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_dsaa.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_mail.c

$(BUILD)/test_link: tests/test_link.c src/libopendectcell/dect_link.c src/libopendectcell/dect_time.c src/libopendectcell/dect_cs.c src/libopendectcell/dect_lc.c src/libopendectcell/dect_lapc.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_mm.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_dsaa.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_link.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -Isrc/libopendectcell -o $@ tests/test_link.c src/libopendectcell/dect_link.c src/libopendectcell/dect_time.c src/libopendectcell/dect_cs.c src/libopendectcell/dect_lc.c src/libopendectcell/dect_lapc.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_mm.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_dsaa.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_mail.c

$(BUILD)/test_profile: tests/test_profile.c src/libopendectcell/dect_profile.c src/libopendectcell/dect_profile.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_profile.c src/libopendectcell/dect_profile.c

$(BUILD)/test_peers: tests/test_peers.c src/baresip/peers.c src/baresip/peers.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_peers.c src/baresip/peers.c

$(BUILD)/test_ctl: tests/test_ctl.c src/libopendectcell/dect_ctl.c src/libopendectcell/dect_ctl.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_ctl.c src/libopendectcell/dect_ctl.c

# Golden traces (tests/traces/): the host stack against recorded output.
TRACE_SRC := src/libopendectcell/dect_link.c src/libopendectcell/dect_time.c src/libopendectcell/dect_cs.c \
	src/libopendectcell/dect_lc.c src/libopendectcell/dect_lapc.c src/libopendectcell/dect_nwk.c \
	src/libopendectcell/dect_mm.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_dsaa.c \
	src/libopendectcell/dect_iwu.c src/libopendectcell/dect_contacts.c src/libopendectcell/dect_calls.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c \
	src/libopendectcell/dect_mail.c
$(BUILD)/test_trace: tests/test_trace.c $(TRACE_SRC) tests/check.h $(wildcard tests/traces/*.trace)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -Isrc/libopendectcell -o $@ tests/test_trace.c $(TRACE_SRC)

$(BUILD)/test_cell: tests/test_cell.c src/libopendectcell/dect_cell.c src/libopendectcell/dect_cell.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_cell.c src/libopendectcell/dect_cell.c

# The baresip module (src/baresip/opendectcell.c), for the box: needs the
# baresip and libre sources of the version on it for their headers, e.g.
#   make baresip-mod BARESIP_SRC=../baresip-3.16.0 RE_SRC=../re-3.16.0
# The OpenWrt package opendectcell-baresip builds it against the feed's headers.
baresip-mod: src/baresip/opendectcell.c src/libopendectcell/dect_ctl.c src/libopendectcell/dect_ctl.h
	@test -n "$(BARESIP_SRC)" -a -n "$(RE_SRC)" || { echo "set BARESIP_SRC= and RE_SRC="; exit 1; }
	@test -x "$(XCC)" || { echo "OpenWrt SDK not found: set SDK="; exit 1; }
	@mkdir -p $(BUILD)
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror -fPIC -shared \
		-DHAVE_INTTYPES_H -I$(RE_SRC)/include -I$(BARESIP_SRC)/include -Isrc/libopendectcell \
		-o $(BUILD)/opendectcell.so src/baresip/opendectcell.c src/baresip/peers.c src/libopendectcell/dect_ctl.c

# dect-cell, the link to an openDECTcell controller: needs mbedTLS 3.6, e.g. for
# the 7412 with the OpenWrt SDK (MBEDTLS = its staging_dir/target-*/usr):
#   make dect-cell CC=<SDK gcc> MBEDTLS=<...>/usr STAGING_DIR=<SDK>/staging_dir
# The OpenWrt package opendectcell-cell builds it against the feed's mbedTLS.
MBEDTLS ?= /usr
CELL_SRC := src/cell/dect-cell.c src/cell/dect_srtp.c src/libopendectcell/dect_cell.c
dect-cell: $(BUILD)/dect-cell
$(BUILD)/dect-cell: $(CELL_SRC) src/libopendectcell/dect_cell.h src/cell/dect_srtp.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(LDFLAGS) -Isrc/libopendectcell -Isrc/cell -I$(MBEDTLS)/include \
		-L$(MBEDTLS)/lib -o $@ $(CELL_SRC) -lmbedtls -lmbedx509 -lmbedcrypto

# dect-cell with the sanitizers against a scripted controller (needs
# MBEDTLS as above, and Python's cryptography): make check-cell MBEDTLS=...
check-cell: $(BUILD)/test_srtp $(BUILD)/dect-cell-test
	./$(BUILD)/test_srtp
	python3 -I tests/test_cell_link.py $(BUILD)/dect-cell-test

$(BUILD)/test_srtp: tests/test_srtp.c src/cell/dect_srtp.c src/cell/dect_srtp.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -Isrc/cell -I$(MBEDTLS)/include -L$(MBEDTLS)/lib \
		-o $@ tests/test_srtp.c -lmbedcrypto

$(BUILD)/dect-cell-test: $(CELL_SRC) src/libopendectcell/dect_cell.h src/cell/dect_srtp.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $(DEFS) -Isrc/libopendectcell -Isrc/cell -I$(MBEDTLS)/include \
		-L$(MBEDTLS)/lib -o $@ $(CELL_SRC) -lmbedtls -lmbedx509 -lmbedcrypto

check: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done
	@python3 -I tests/test_fw_extract.py
	@python3 -I tests/test_luci_js.py
	@python3 -I tests/test_firmware_sh.py
	@python3 -I tests/test_package_keep.py
	@python3 -I tests/test_release_files.py
	@python3 -I tests/test_sip_init.py
	@python3 -I tests/test_luci_acl.py
	@python3 -I tests/test_enroll_start.py

clean:
	rm -rf $(BUILD)
