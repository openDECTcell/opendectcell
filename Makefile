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

TESTS := $(BUILD)/test_fwimage

.PHONY: all check clean
all: $(TESTS)

$(BUILD)/test_fwimage: tests/test_fwimage.c src/fwload/fwimage.c src/fwload/fwimage.h tests/check.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ tests/test_fwimage.c src/fwload/fwimage.c

check: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

clean:
	rm -rf $(BUILD)
