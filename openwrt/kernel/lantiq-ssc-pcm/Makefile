# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lantiq-ssc-pcm
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/openDECTcell/kmod-lantiq-ssc-pcm.git
PKG_SOURCE_DATE:=2026-11-08
PKG_SOURCE_VERSION:=829a378daae951f02cf492cad7cddf3dedfd4957
PKG_MIRROR_HASH:=9458deb7ea1671a15041e3efb7f68767ec8bdade068f1e1d5f55fab377591759

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=ctandi <amo@ct.de>

include $(INCLUDE_DIR)/package.mk

define KernelPackage/lantiq-ssc-pcm
  SUBMENU:=Voice over IP
  TITLE:=Lantiq VR9 SSC PCM/TDM transport
  URL:=https://github.com/openDECTcell/kmod-lantiq-ssc-pcm
  DEPENDS:=@TARGET_lantiq_xrx200
  FILES:=$(PKG_BUILD_DIR)/src/lantiq_ssc_pcm.ko
endef

define KernelPackage/lantiq-ssc-pcm/description
  The Lantiq VR9 Synchronous Serial Controller as a PCM/TDM slave (clock
  and frame sync from the peer), through /dev/ssc-pcm0. Used by opendectcell on
  the FRITZ!Box 7412. Not loaded at boot; its user loads it.
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)/src" modules
endef

$(eval $(call KernelPackage,lantiq-ssc-pcm))
