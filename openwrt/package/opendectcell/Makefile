# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk

PKG_NAME:=opendectcell
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

# Package location in the repository: openwrt/package/opendectcell
OPENDECTCELL_SRC:=$(abspath $(CURDIR)/../../..)

include $(INCLUDE_DIR)/package.mk

define Package/opendectcell
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=DECT base station for the FRITZ!Box 7520/7530
  DEPENDS:=@TARGET_ipq40xx +kmod-opendectcell-ipq40xx
endef

define Package/opendectcell/description
  Runs the box's DECT chip as a GAP/CAT-iq base: dect-fwload boots the
  chip from user-supplied AVM firmware, dect-stubd runs registration, the
  NWK protocol stack and calls. Disabled after installation: nothing is
  loaded and nothing transmits until /etc/config/opendectcell says enabled.
endef

define Package/opendectcell/conffiles
/etc/config/opendectcell
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/kernel
	$(CP) $(OPENDECTCELL_SRC)/Makefile $(OPENDECTCELL_SRC)/src $(PKG_BUILD_DIR)/
	$(CP) $(OPENDECTCELL_SRC)/kernel/include $(PKG_BUILD_DIR)/kernel/
endef

# The repository Makefile's own rules, with the target compiler. -Werror
# stays off here: a newer toolchain's new warning must not break a package
# build; the host build keeps it.
define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -std=c11 -Wall -Wextra" \
		build/dect-stubd build/dect-fwload build/dect-pcmcap
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
endef

$(eval $(call BuildPackage,opendectcell))
