# SPDX-License-Identifier: GPL-2.0-or-later
#
# uclient's ucode module with one change (patches/): a request body whose
# length the caller gives in a Content-Length header goes out as it is,
# not chunked, which some CardDAV servers need (docs/phonebook.md). Built
# as a module of its own, opendectcell_uclient, from the uclient commit the
# base feed packages, so the system's libuclient stays as it is.

include $(TOPDIR)/rules.mk

PKG_NAME:=ucode-mod-opendectcell-uclient
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL=$(PROJECT_GIT)/project/uclient.git
PKG_SOURCE_DATE:=2026-06-07
PKG_SOURCE_VERSION:=daad21fa2c17cae54d37149bf27bb3dd78e50a3f
# the base feed's own tarball, so its mirror hash holds
PKG_SOURCE_SUBDIR:=uclient-$(subst -,.,$(PKG_SOURCE_DATE))~$(call version_abbrev,$(PKG_SOURCE_VERSION))
PKG_SOURCE:=$(PKG_SOURCE_SUBDIR).tar.zst
PKG_MIRROR_HASH:=35c51545ee092b816249799d283838ffba904292636c495818d864c56b554fe0
PKG_UNPACK=zstdcat $(DL_DIR)/$(PKG_SOURCE) | \
	$(HOST_TAR) -C $(PKG_BUILD_DIR) --strip-components=1 -xf -

PKG_BUILD_DEPENDS:=ucode
PKG_LICENSE:=ISC
PKG_LICENSE_FILES:=

include $(INCLUDE_DIR)/package.mk

define Package/ucode-mod-opendectcell-uclient
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=ucode HTTP client for opendectcell (uclient, sized request bodies)
  URL:=https://github.com/openDECTcell/opendectcell
  DEPENDS:=+libubox +libucode
endef

define Package/ucode-mod-opendectcell-uclient/description
  uclient's ucode module as "opendectcell_uclient", changed to send a request
  body of known length without chunked transfer coding. Used by
  opendectcell-carddav.
endef

define Build/Configure
endef

# uclient's library and its ucode binding in one module; ucode resolves
# the ucode symbols at load, as for its own modules
define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=gnu99 -Wall -fPIC \
		-shared -o $(PKG_BUILD_DIR)/opendectcell_uclient.so \
		$(addprefix $(PKG_BUILD_DIR)/,uclient.c uclient-http.c uclient-utils.c ucode.c) \
		$(TARGET_LDFLAGS) -lubox -ldl
endef

define Package/ucode-mod-opendectcell-uclient/install
	$(INSTALL_DIR) $(1)/usr/lib/ucode
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/opendectcell_uclient.so $(1)/usr/lib/ucode/
endef

$(eval $(call BuildPackage,ucode-mod-opendectcell-uclient))
