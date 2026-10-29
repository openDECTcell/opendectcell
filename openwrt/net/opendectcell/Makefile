# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=opendectcell
PKG_VERSION:=0.3.1
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/openDECTcell/opendectcell/tar.gz/v$(PKG_VERSION)?
PKG_HASH:=92249b76d3ff58d30f2e88d192c8ce956233f87a12451a32f416d57c83669c1e

PKG_LICENSE:=GPL-2.0-or-later GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=ctandi <amo@ct.de>

PKG_BUILD_DEPENDS:=PACKAGE_opendectcell-baresip:baresip
PKG_CONFIG_DEPENDS:=CONFIG_PACKAGE_opendectcell-baresip
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/opendectcell/Default
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  URL:=https://github.com/openDECTcell/opendectcell
endef

define Package/opendectcell
  $(call Package/opendectcell/Default)
  TITLE:=DECT base station for the FRITZ!Box 7520/7530 and 7412
  DEPENDS:=@(TARGET_ipq40xx||TARGET_lantiq) \
	+TARGET_ipq40xx:kmod-opendectcell-ipq40xx \
	+TARGET_lantiq:kmod-opendectcell-lantiq \
	+TARGET_lantiq:kmod-lantiq-ssc-pcm
endef

define Package/opendectcell/description
  Runs the box's DECT chip as a GAP/CAT-iq base: dect-fwload boots the
  chip from user-supplied AVM firmware, dect-stubd runs registration, the
  NWK protocol stack and calls. Disabled after installation: nothing is
  loaded and nothing transmits until /etc/config/opendectcell says enabled.
endef

define Package/opendectcell/conffiles
/etc/config/opendectcell
/etc/opendectcell/profiles/
endef

define Package/opendectcell-baresip
  $(call Package/opendectcell/Default)
  TITLE:=SIP for the opendectcell DECT base (baresip module)
  DEPENDS:=+opendectcell +baresip +baresip-mod-g711 +baresip-mod-g722 \
	+baresip-mod-auresamp +baresip-mod-srtp
endef

define Package/opendectcell-baresip/description
  The baresip module "opendectcell": the bridge between dect-stubd and SIP.
  Each handset's Line 1 and Line 2 (Network > DECT, Handsets) become
  baresip accounts; the DECT service starts baresip with them when at
  least one line is switched on.
endef

define KernelPackage/opendectcell-ipq40xx
  SUBMENU:=Voice over IP
  TITLE:=FRITZ!Box 7520/7530 DECT chip control and PCM/TDM
  URL:=https://github.com/openDECTcell/opendectcell
  DEPENDS:=@TARGET_ipq40xx
  FILES:=$(PKG_BUILD_DIR)/kernel/opendectcell.ko
endef

define KernelPackage/opendectcell-ipq40xx/description
  Kernel module for opendectcell on the FRITZ!Box 7520/7530: the DECT chip's
  reset and the PCM/TDM link through the IPQ4019 audio block
  (/dev/dect0). Not loaded at boot; the opendectcell service loads it.
endef

define KernelPackage/opendectcell-lantiq
  SUBMENU:=Voice over IP
  TITLE:=FRITZ!Box 7412 DECT chip control
  URL:=https://github.com/openDECTcell/opendectcell
  DEPENDS:=@TARGET_lantiq
  FILES:=$(PKG_BUILD_DIR)/kernel/opendectcell.ko
endef

define KernelPackage/opendectcell-lantiq/description
  Kernel module for opendectcell on the FRITZ!Box 7412: the DECT chip's reset
  (/dev/dect0). The PCM/TDM link is kmod-lantiq-ssc-pcm. Not loaded at
  boot; the opendectcell service loads it.
endef

ifdef CONFIG_TARGET_lantiq
  OPENDECTCELL_KMOD_ARGS:=OPENDECTCELL_LANTIQ=1
endif

define Build/Compile/baresip
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=gnu11 -Wall -Wextra -fPIC -shared -DHAVE_INTTYPES_H \
		-I$(STAGING_DIR)/usr/include/re -I$(PKG_BUILD_DIR)/src/libopendectcell \
		-o $(PKG_BUILD_DIR)/build/opendectcell.so \
		$(PKG_BUILD_DIR)/src/baresip/opendectcell.c \
		$(PKG_BUILD_DIR)/src/libopendectcell/dect_ctl.c
endef

# The project Makefile's own rules with the target toolchain. -Werror
# stays off here: a newer compiler's new warning must not break the
# package; the project's host build keeps it.
define Build/Compile
	+$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 -Wall -Wextra" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		VERSION="$(PKG_VERSION)" \
		build/dect-stubd build/dect-fwload build/dect-pcmcap
	$(if $(CONFIG_PACKAGE_opendectcell-baresip),$(call Build/Compile/baresip))
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)/kernel" \
		$(OPENDECTCELL_KMOD_ARGS) modules
endef

define Package/opendectcell/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/dect-stubd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/dect-fwload $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/dect-pcmcap $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config $(1)/etc/opendectcell/firmware
	$(INSTALL_BIN) ./files/opendectcell.init $(1)/etc/init.d/opendectcell
	$(INSTALL_CONF) ./files/opendectcell.config $(1)/etc/config/opendectcell
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/opendectcell.keep $(1)/lib/upgrade/keep.d/opendectcell
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/opendectcell.uci-defaults \
		$(1)/etc/uci-defaults/90-opendectcell
	$(INSTALL_BIN) ./files/opendectcell.voip-migrate \
		$(1)/etc/uci-defaults/91-opendectcell-voip
	$(INSTALL_DIR) $(1)/usr/share/opendectcell/boards
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/boards/avm,fritzbox-7530 \
		$(PKG_BUILD_DIR)/boards/avm,fritz7412 \
		$(1)/usr/share/opendectcell/boards/
	$(INSTALL_DIR) $(1)/etc/opendectcell/profiles
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/profiles/*.profile \
		$(1)/etc/opendectcell/profiles/
endef

define Package/opendectcell-baresip/install
	$(INSTALL_DIR) $(1)/usr/lib/baresip/modules
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/opendectcell.so \
		$(1)/usr/lib/baresip/modules/
endef

$(eval $(call BuildPackage,opendectcell))
$(eval $(call BuildPackage,opendectcell-baresip))
$(eval $(call KernelPackage,opendectcell-ipq40xx))
$(eval $(call KernelPackage,opendectcell-lantiq))
