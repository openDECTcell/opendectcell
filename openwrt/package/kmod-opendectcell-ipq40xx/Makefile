# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=opendectcell-ipq40xx
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

# Package location in the repository: openwrt/package/kmod-opendectcell-ipq40xx
OPENDECTCELL_KERNEL_SRC:=$(abspath $(CURDIR)/../../../kernel)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/kernel-defaults.mk

define KernelPackage/opendectcell-ipq40xx
  SUBMENU:=Other modules
  TITLE:=FRITZ!Box 7520/7530 DECT chip control and PCM/TDM
  DEPENDS:=@TARGET_ipq40xx
  FILES:=$(PKG_BUILD_DIR)/opendectcell.ko
  # Deliberately no AUTOLOAD: only manual insmod is permitted.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/include/uapi
	$(CP) $(OPENDECTCELL_KERNEL_SRC)/*.c $(OPENDECTCELL_KERNEL_SRC)/*.h \
		$(OPENDECTCELL_KERNEL_SRC)/Makefile $(PKG_BUILD_DIR)/
	$(CP) $(OPENDECTCELL_KERNEL_SRC)/include/uapi/opendectcell.h \
		$(PKG_BUILD_DIR)/include/uapi/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,opendectcell-ipq40xx))
