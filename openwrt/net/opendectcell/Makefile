# SPDX-License-Identifier: GPL-2.0-or-later
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=opendectcell
PKG_VERSION:=0.3.3
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/openDECTcell/opendectcell/tar.gz/v$(PKG_VERSION)?
PKG_HASH:=92249b76d3ff58d30f2e88d192c8ce956233f87a12451a32f416d57c83669c1e

PKG_LICENSE:=GPL-2.0-or-later GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=ctandi <amo@ct.de>

PKG_BUILD_DEPENDS:=PACKAGE_opendectcell-baresip:baresip PACKAGE_opendectcell-cell:mbedtls
PKG_CONFIG_DEPENDS:=CONFIG_PACKAGE_opendectcell-baresip CONFIG_PACKAGE_opendectcell-cell
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
  DEPENDS:=@(TARGET_ipq40xx||TARGET_lantiq_xrx200) \
	+TARGET_ipq40xx:kmod-opendectcell-ipq40xx \
	+TARGET_lantiq_xrx200:kmod-opendectcell-lantiq \
	+TARGET_lantiq_xrx200:kmod-lantiq-ssc-pcm
endef

define Package/opendectcell/description
  Runs the box's DECT chip as a GAP/CAT-iq base: dect-fwload boots the
  chip from user-supplied AVM firmware, dect-stubd runs registration, the
  NWK protocol stack and calls. Disabled after installation: nothing is
  loaded and nothing transmits until /etc/config/opendectcell says enabled.
endef

# The FRITZ!Box 7362 SL's own transports, kept out of openDECTcell's DEPENDS so
# that a 7412 (the same target) never installs them. The 7412 keeps
# getting kmod-lantiq-ssc-pcm through openDECTcell as it has since 2026-10-29:
# moving that into a package of its own could have apk drop the module
# from a 7412 that upgrades openDECTcell alone.
define Package/opendectcell-fritz7362sl
  $(call Package/opendectcell/Default)
  TITLE:=opendectcell for the FRITZ!Box 7362 SL (its transport modules)
  DEPENDS:=@TARGET_lantiq_xrx200 +opendectcell \
	+kmod-lantiq-tdm-pcm +kmod-lantiq-usif-uart
endef

define Package/opendectcell-fritz7362sl/description
  Installs opendectcell with the kernel modules the FRITZ!Box 7362 SL needs:
  kmod-lantiq-usif-uart for the chip's firmware upload and
  kmod-lantiq-tdm-pcm for its PCM link. Install this one, not opendectcell
  alone, on a 7362 SL.
endef

define Package/opendectcell/conffiles
/etc/config/opendectcell
/etc/opendectcell/profiles/
endef

define Package/opendectcell-baresip
  USERID:=opendectcell:opendectcell
  $(call Package/opendectcell/Default)
  TITLE:=SIP for the opendectcell DECT base (baresip module)
  DEPENDS:=+opendectcell +baresip +baresip-mod-g711 +baresip-mod-auresamp \
	+baresip-mod-mwi +baresip-mod-srtp
endef

define Package/opendectcell-baresip/description
  The baresip module "opendectcell": the bridge between dect-stubd and SIP.
  Each VoIP profile that is a handset's Line 1 or Line 2 (Network >
  DECT) becomes one baresip account, shared by every handset that has
  it as a line; the DECT service starts baresip when at least one
  handset has a line.
endef

define Package/opendectcell-cell
  $(call Package/opendectcell/Default)
  TITLE:=opendectcell cell: the link to an openDECTcell controller
  DEPENDS:=+opendectcell +libmbedtls
endef

define Package/opendectcell-cell/description
  dect-cell: makes the box a cell of an openDECTcell controller (TLS 1.3 with
  mutual authentication, enrolment with a one-time token). In cell mode
  the box runs no DECT stack and no SIP of its own: the controller does.
  Switched on with option role 'cell' in /etc/config/opendectcell, or by
  /etc/init.d/opendectcell enroll '<enrollment string>'.
endef

define Package/opendectcell-carddav
  $(call Package/opendectcell/Default)
  TITLE:=opendectcell phonebook from a CardDAV address book
  DEPENDS:=+opendectcell +ucode +ucode-mod-fs +ucode-mod-uci +ucode-mod-uloop \
	+ucode-mod-opendectcell-uclient +ca-bundle
  PKGARCH:=all
endef

define Package/opendectcell-carddav/description
  The handsets' phonebook (CAT-iq Contact List) from a CardDAV address
  book over HTTPS, both ways: what a handset adds, edits or deletes is
  written back to the server. Configured in /etc/config/opendectcell
  (config carddav) or in LuCI, Network > DECT.
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
  TITLE:=FRITZ!Box 7412 and 7362 SL DECT chip control
  URL:=https://github.com/openDECTcell/opendectcell
  DEPENDS:=@TARGET_lantiq_xrx200
  FILES:=$(PKG_BUILD_DIR)/kernel/opendectcell.ko
endef

define KernelPackage/opendectcell-lantiq/description
  Kernel module for opendectcell on the FRITZ!Box 7412 and 7362 SL: the DECT
  chip's reset (/dev/dect0). On the 7412 the PCM/TDM link is
  kmod-lantiq-ssc-pcm, on the 7362 SL kmod-lantiq-tdm-pcm. Not loaded at
  boot; the opendectcell service loads it.
endef

ifdef CONFIG_TARGET_lantiq_xrx200
  OPENDECTCELL_KMOD_ARGS:=OPENDECTCELL_LANTIQ=1
endif

# A board profile ships with the target it runs on, so a 7520 does not carry the
# 7412's and the other way round. The 7362 SL's rides in its own package, below.
ifdef CONFIG_TARGET_ipq40xx
  OPENDECTCELL_BOARDS:=avm,fritzbox-7530
endif
ifdef CONFIG_TARGET_lantiq_xrx200
  OPENDECTCELL_BOARDS:=avm,fritz7412
endif

define Build/Compile/baresip
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-std=gnu11 -Wall -Wextra -fPIC -shared -DHAVE_INTTYPES_H \
		-I$(STAGING_DIR)/usr/include/re -I$(PKG_BUILD_DIR)/src/libopendectcell \
		-o $(PKG_BUILD_DIR)/build/opendectcell.so \
		$(PKG_BUILD_DIR)/src/baresip/opendectcell.c \
		$(PKG_BUILD_DIR)/src/baresip/peers.c \
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
		build/dect-stubd build/dect-fwload
	$(if $(CONFIG_PACKAGE_opendectcell-baresip),$(call Build/Compile/baresip))
	$(if $(CONFIG_PACKAGE_opendectcell-cell),+$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 -Wall -Wextra" \
		LDFLAGS="$(TARGET_LDFLAGS)" VERSION="$(PKG_VERSION)" \
		MBEDTLS="$(STAGING_DIR)/usr" build/dect-cell)
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)/kernel" \
		$(OPENDECTCELL_KMOD_ARGS) modules
endef

define Package/opendectcell/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/dect-stubd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/dect-fwload $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config $(1)/etc/opendectcell/firmware
	$(INSTALL_BIN) ./files/opendectcell.init $(1)/etc/init.d/opendectcell
	$(INSTALL_CONF) ./files/opendectcell.config $(1)/etc/config/opendectcell
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/opendectcell.keep $(1)/lib/upgrade/keep.d/opendectcell
	$(INSTALL_DIR) $(1)/etc/hotplug.d/ntp
	$(INSTALL_DATA) ./files/opendectcell.ntp-hotplug \
		$(1)/etc/hotplug.d/ntp/25-opendectcell
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/opendectcell.uci-defaults \
		$(1)/etc/uci-defaults/90-opendectcell
	$(INSTALL_BIN) ./files/opendectcell.voip-migrate \
		$(1)/etc/uci-defaults/91-opendectcell-voip
	$(INSTALL_DIR) $(1)/usr/share/opendectcell/boards
	$(INSTALL_BIN) ./files/firmware.sh $(1)/usr/share/opendectcell/firmware.sh
	$(INSTALL_DATA) $(addprefix $(PKG_BUILD_DIR)/boards/,$(OPENDECTCELL_BOARDS)) \
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
# The cell's key, certificate, CA and enrolment live in /etc/opendectcell/cell/: a
# sysupgrade has to keep them, or every cell needs a new enrolment (a site
# visit) after a firmware upgrade.
define Package/opendectcell-cell/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/dect-cell $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/opendectcell-cell.keep $(1)/lib/upgrade/keep.d/opendectcell-cell
endef

# Its board profile, and the dependencies (the point of the package).
define Package/opendectcell-fritz7362sl/install
	$(INSTALL_DIR) $(1)/usr/share/opendectcell/boards
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/boards/avm,fritz7362sl $(1)/usr/share/opendectcell/boards/
endef

define Package/opendectcell-carddav/install
	$(INSTALL_DIR) $(1)/usr/share/opendectcell
	$(INSTALL_BIN) ./files/carddav.uc $(1)/usr/share/opendectcell/carddav.uc
endef

$(eval $(call BuildPackage,opendectcell-baresip))
$(eval $(call BuildPackage,opendectcell-cell))
$(eval $(call BuildPackage,opendectcell-carddav))
$(eval $(call BuildPackage,opendectcell-fritz7362sl))
$(eval $(call KernelPackage,opendectcell-ipq40xx))
$(eval $(call KernelPackage,opendectcell-lantiq))
