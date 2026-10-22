# SPDX-License-Identifier: GPL-2.0-or-later
# Host build and tests. OpenWrt packages use their own Makefiles (openwrt/).

CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD   := build

# Local AVM files for the optional real-file tests (never in git).
FW7520  := work/7520/rootfs/lib/firmware
export OPENDECTCELL_STUB_HEX ?= $(FW7520)/dectfw_secondlevel_441.hex
export OPENDECTCELL_FL_HEX   ?= $(FW7520)/dectfw_firstlevel_441_slow.hex

TESTS := $(BUILD)/test_fwimage $(BUILD)/test_mail $(BUILD)/test_session $(BUILD)/test_audio $(BUILD)/test_cs $(BUILD)/test_lc $(BUILD)/test_lapc $(BUILD)/test_nwk $(BUILD)/test_dsaa $(BUILD)/test_mm $(BUILD)/test_cc $(BUILD)/test_iwu $(BUILD)/test_g722 $(BUILD)/test_clip $(BUILD)/test_callinfo
FWLOAD_SRC := src/fwload/dect-fwload.c src/fwload/loader.c src/fwload/fwimage.c
LIBDECT_SRC := src/libopendectcell/dect_mail.c src/libopendectcell/dect_session.c src/libopendectcell/dect_audio.c \
	src/libopendectcell/dect_g722.c

# Cross build for the 7520 with the OpenWrt SDK (25.12.5, ipq40xx/generic).
SDK     ?= $(firstword $(wildcard $(HOME)/opt/openwrt-sdk-25.12.5-ipq40xx-*))
TCBIN   := $(firstword $(wildcard $(SDK)/staging_dir/toolchain-*/bin))
XCC     := $(TCBIN)/arm-openwrt-linux-gcc

# dect-stubd: framing, session, the C-plane stack, and the A-law codec for the
# transmit test tone (dect_audio.c pulls in libm).
STUBD_SRC := src/stubd/dect-stubd.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_session.c \
	src/libopendectcell/dect_cs.c src/libopendectcell/dect_lc.c src/libopendectcell/dect_lapc.c \
	src/libopendectcell/dect_nwk.c src/libopendectcell/dect_dsaa.c src/libopendectcell/dect_mm.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c \
	src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_audio.c \
	src/libopendectcell/dect_g722.c
STUBD_INC := -Ikernel/include/uapi -Isrc/libopendectcell

.PHONY: all check cross clean
all: $(TESTS) $(BUILD)/dect-fwload $(BUILD)/dect-pcmcap $(BUILD)/dect-stubd $(BUILD)/dect-maildump

$(BUILD)/dect-fwload: $(FWLOAD_SRC) src/fwload/loader.h src/fwload/fwimage.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(FWLOAD_SRC)

# Host build of dect-pcmcap for offline analysis (-S FILE) of captures.
$(BUILD)/dect-pcmcap: src/tools/dect-pcmcap.c src/libopendectcell/dect_audio.c src/libopendectcell/dect_audio.h kernel/include/uapi/opendectcell.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ikernel/include/uapi -Isrc/libopendectcell -o $@ src/tools/dect-pcmcap.c src/libopendectcell/dect_audio.c -lm

# Host build of dect-stubd: compiles everywhere, runs only on the box.
# Offline: recover the full mail stream from a `dect-stubd -w` capture. The
# control slot is in every capture, so this works on runs already taken.
$(BUILD)/dect-maildump: src/tools/dect-maildump.c src/libopendectcell/dect_mail.c src/libopendectcell/dect_mail.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc/libopendectcell -o $@ src/tools/dect-maildump.c src/libopendectcell/dect_mail.c

$(BUILD)/dect-stubd: $(STUBD_SRC) src/libopendectcell/dect_mail.h src/libopendectcell/dect_session.h src/libopendectcell/dect_proto.h kernel/include/uapi/opendectcell.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(STUBD_INC) -o $@ $(STUBD_SRC) -lm

# Result: build/dect-fwload.arm (stripped), ready to copy to the box.
cross: $(FWLOAD_SRC)
	@test -x "$(XCC)" || { echo "OpenWrt SDK not found: set SDK="; exit 1; }
	@mkdir -p $(BUILD)
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror -Wshadow \
		-o $(BUILD)/dect-fwload.arm.dbg $(FWLOAD_SRC)
	$(TCBIN)/arm-openwrt-linux-muslgnueabi-strip -o $(BUILD)/dect-fwload.arm $(BUILD)/dect-fwload.arm.dbg
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror -Ikernel/include/uapi \
		-o $(BUILD)/dect-sla.arm src/tools/dect-sla.c
	$(TCBIN)/arm-openwrt-linux-muslgnueabi-strip $(BUILD)/dect-sla.arm
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror -Ikernel/include/uapi -Isrc/libopendectcell \
		-o $(BUILD)/dect-pcmcap.arm src/tools/dect-pcmcap.c src/libopendectcell/dect_audio.c -lm
	$(TCBIN)/arm-openwrt-linux-muslgnueabi-strip $(BUILD)/dect-pcmcap.arm
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror $(STUBD_INC) \
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

$(BUILD)/test_iwu: tests/test_iwu.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_iwu.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_iwu.c src/libopendectcell/dect_iwu.c

$(BUILD)/test_cc: tests/test_cc.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_cc.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_cc.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_callinfo.c

$(BUILD)/test_clip: tests/test_clip.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_clip.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_clip.c src/libopendectcell/dect_clip.c src/libopendectcell/dect_cc.c src/libopendectcell/dect_iwu.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_callinfo.c

$(BUILD)/test_callinfo: tests/test_callinfo.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_nwk.c src/libopendectcell/dect_callinfo.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_callinfo.c src/libopendectcell/dect_callinfo.c src/libopendectcell/dect_nwk.c

# Conformance against ffmpeg is a separate, optional step: tools/g722-crosscheck.sh
$(BUILD)/test_g722: tests/test_g722.c src/libopendectcell/dect_g722.c src/libopendectcell/dect_g722.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_g722.c src/libopendectcell/dect_g722.c -lm

check: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

clean:
	rm -rf $(BUILD)
