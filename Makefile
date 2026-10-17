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

TESTS := $(BUILD)/test_fwimage $(BUILD)/test_mail
FWLOAD_SRC := src/fwload/dect-fwload.c src/fwload/loader.c src/fwload/fwimage.c
LIBDECT_SRC := src/libopendectcell/dect_mail.c

# Cross build for the 7520 with the OpenWrt SDK (25.12.5, ipq40xx/generic).
SDK     ?= $(firstword $(wildcard $(HOME)/opt/openwrt-sdk-25.12.5-ipq40xx-*))
TCBIN   := $(firstword $(wildcard $(SDK)/staging_dir/toolchain-*/bin))
XCC     := $(TCBIN)/arm-openwrt-linux-gcc

.PHONY: all check cross clean
all: $(TESTS) $(BUILD)/dect-fwload $(BUILD)/dect-pcmcap

$(BUILD)/dect-fwload: $(FWLOAD_SRC) src/fwload/loader.h src/fwload/fwimage.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(FWLOAD_SRC)

# Host build of dect-pcmcap for offline analysis (-S FILE) of captures.
$(BUILD)/dect-pcmcap: src/tools/dect-pcmcap.c kernel/include/uapi/opendectcell.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ikernel/include/uapi -o $@ src/tools/dect-pcmcap.c

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
	STAGING_DIR=$(dir $(TCBIN))/.. $(XCC) -std=gnu11 -Os -Wall -Wextra -Werror -Ikernel/include/uapi \
		-o $(BUILD)/dect-pcmcap.arm src/tools/dect-pcmcap.c
	$(TCBIN)/arm-openwrt-linux-muslgnueabi-strip $(BUILD)/dect-pcmcap.arm

$(BUILD)/test_fwimage: tests/test_fwimage.c src/fwload/fwimage.c src/fwload/fwimage.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_fwimage.c src/fwload/fwimage.c

$(BUILD)/test_mail: tests/test_mail.c $(LIBDECT_SRC) src/libopendectcell/dect_mail.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_mail.c $(LIBDECT_SRC)

check: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

clean:
	rm -rf $(BUILD)
