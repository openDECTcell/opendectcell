# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lantiq-usif-uart
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/openDECTcell/kmod-lantiq-usif-uart.git
PKG_SOURCE_DATE:=2026-11-01
PKG_SOURCE_VERSION:=f3591066c33cb4a007c9fde19d9596200b7e2468
# Not computed yet: the repository is private and was not reachable from
# the build host. Set it from `make package/lantiq-usif-uart/check FIXUP=1`
# once it is.
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=GPL-2.0-or-later GPL-2.0-only
PKG_MAINTAINER:=ctandi <amo@ct.de>

include $(INCLUDE_DIR)/package.mk

define KernelPackage/lantiq-usif-uart
  SUBMENU:=Voice over IP
  TITLE:=Lantiq VR9 USIF UART (ttyUSIF)
  URL:=https://github.com/openDECTcell/kmod-lantiq-usif-uart
  DEPENDS:=@TARGET_lantiq_xrx200
  FILES:=$(PKG_BUILD_DIR)/src/lantiq_usif_uart.ko
endef

define KernelPackage/lantiq-usif-uart/description
  The Lantiq VR9 USIF as a UART, through /dev/ttyUSIF0. Used by opendectcell
  to load the DECT chip's firmware on the FRITZ!Box 7362 SL, whose chip
  is wired to the USIF. Not loaded at boot; its user loads it.
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)/src" modules
endef

$(eval $(call KernelPackage,lantiq-usif-uart))
