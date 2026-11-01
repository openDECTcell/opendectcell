# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lantiq-tdm-pcm
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/openDECTcell/kmod-lantiq-tdm-pcm.git
PKG_SOURCE_DATE:=2026-11-01
PKG_SOURCE_VERSION:=44508910c4375de38e4b4e89f6138457056f035e
# Not computed yet: the repository is private and was not reachable from
# the build host. Set it from `make package/lantiq-tdm-pcm/check FIXUP=1`
# once it is.
PKG_MIRROR_HASH:=skip

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
