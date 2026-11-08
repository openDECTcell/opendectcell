# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lantiq-usif-uart
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/openDECTcell/kmod-lantiq-usif-uart.git
PKG_SOURCE_DATE:=2026-11-08
PKG_SOURCE_VERSION:=1789c38ff01ca958bb2c05a6df46eb44241b7736
PKG_MIRROR_HASH:=447e6ef96b3a93a896e1aefb492f81e653c7ce3d875d708f3494ea7103f9a870

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
