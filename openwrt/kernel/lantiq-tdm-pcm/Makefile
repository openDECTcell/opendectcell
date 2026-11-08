# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lantiq-tdm-pcm
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/openDECTcell/kmod-lantiq-tdm-pcm.git
PKG_SOURCE_DATE:=2026-11-08
PKG_SOURCE_VERSION:=4b3a55ed8a33f6679c65084ef551d0b93c1e8741
PKG_MIRROR_HASH:=98a60bcbd6b14c1b4a9f6daea12df389e96a51be93b1644da3b6c575a15a85cb

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=ctandi <amo@ct.de>

include $(INCLUDE_DIR)/package.mk

define KernelPackage/lantiq-tdm-pcm
  SUBMENU:=Voice over IP
  TITLE:=Lantiq VR9 TDM PCM transport
  URL:=https://github.com/openDECTcell/kmod-lantiq-tdm-pcm
  DEPENDS:=@TARGET_lantiq_xrx200
  FILES:=$(PKG_BUILD_DIR)/src/lantiq_tdm_pcm.ko
endef

define KernelPackage/lantiq-tdm-pcm/description
  The Lantiq VR9 TDM block as a PCM slave (clock and frame sync from the
  peer), through /dev/tdm-pcm0, with the same interface as
  kmod-lantiq-ssc-pcm. Used by opendectcell on the FRITZ!Box 7362 SL. Not
  loaded at boot; its user loads it.
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)/src" modules
endef

$(eval $(call KernelPackage,lantiq-tdm-pcm))
