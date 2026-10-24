# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-opendectcell
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-opendectcell
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=DECT Monitor for opendectcell
  DEPENDS:=+luci-base +opendectcell
  PKGARCH:=all
endef

define Package/luci-app-opendectcell/description
  Status > DECT: the base's radio state and, per registered handset, its
  link, last contact, carrier and field strength, read from the state file
  dect-stubd writes (/tmp/opendectcell-state.json).
endef

define Build/Compile
endef

define Package/luci-app-opendectcell/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/opendectcell
	$(INSTALL_DATA) ./files/monitor.js \
		$(1)/www/luci-static/resources/view/opendectcell/monitor.js
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./files/menu.json \
		$(1)/usr/share/luci/menu.d/luci-app-opendectcell.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/acl.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-opendectcell.json
endef

$(eval $(call BuildPackage,luci-app-opendectcell))
