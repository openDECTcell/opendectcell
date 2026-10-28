# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk
include $(CURDIR)/../../version.mk

PKG_NAME:=opendectcell-baresip
PKG_VERSION:=$(OPENDECTCELL_VERSION)
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_BUILD_DEPENDS:=baresip libre

# Package location in the repository: openwrt/package/opendectcell-baresip
OPENDECTCELL_SRC:=$(abspath $(CURDIR)/../../..)

include $(INCLUDE_DIR)/package.mk

define Package/opendectcell-baresip
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=SIP for the opendectcell DECT base (baresip module)
  DEPENDS:=+opendectcell +baresip +baresip-mod-g711 +baresip-mod-g722 \
	+baresip-mod-srtp
endef

define Package/opendectcell-baresip/description
  The baresip module "opendectcell": the bridge between dect-stubd and SIP.
  Each handset's Line 1 and Line 2 (Network > DECT, Handsets) become
  baresip accounts; the DECT service starts baresip with them when at
  least one line is switched on.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/src
	$(CP) $(OPENDECTCELL_SRC)/src/baresip $(OPENDECTCELL_SRC)/src/libopendectcell $(PKG_BUILD_DIR)/src/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -fPIC -shared \
		-DHAVE_INTTYPES_H -I$(STAGING_DIR)/usr/include/re \
		-I$(STAGING_DIR)/usr/include -I$(PKG_BUILD_DIR)/src/libopendectcell \
		-o $(PKG_BUILD_DIR)/opendectcell.so \
		$(PKG_BUILD_DIR)/src/baresip/opendectcell.c \
		$(PKG_BUILD_DIR)/src/libopendectcell/dect_ctl.c
endef

define Package/opendectcell-baresip/install
	$(INSTALL_DIR) $(1)/usr/lib/baresip/modules
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/opendectcell.so $(1)/usr/lib/baresip/modules/
endef

$(eval $(call BuildPackage,opendectcell-baresip))
