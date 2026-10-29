# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk

LUCI_TITLE:=DECT settings and Monitor for opendectcell
LUCI_DESCRIPTION:=Network > DECT: the base's settings and its handsets \
	(rename, profile, register, deregister). Status > DECT: the DECT \
	Monitor, per base, handset and connection.
LUCI_DEPENDS:=+luci-base +opendectcell
LUCI_PKGARCH:=all

PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=ctandi <amo@ct.de>

include ../../luci.mk

# call BuildPackage - OpenWrt buildroot signature
