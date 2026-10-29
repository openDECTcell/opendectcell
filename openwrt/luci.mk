# SPDX-License-Identifier: GPL-2.0-or-later
#
# Not part of any submission. In the LuCI repository an application's
# Makefile includes ../../luci.mk, the LuCI feed's own build rules; here
# the same line reaches this file, which hands over to the LuCI feed of
# the build tree (./scripts/feeds update luci). That way
# applications/luci-app-opendectcell/Makefile stays exactly as it would be
# submitted.
include $(TOPDIR)/feeds/luci/luci.mk
