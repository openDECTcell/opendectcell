# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(CURDIR)/../../version.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=opendectcell-lantiq
PKG_VERSION:=$(OPENDECTCELL_VERSION)
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

# Package location in the repository: openwrt/package/kmod-opendectcell-lantiq
OPENDECTCELL_KERNEL_SRC:=$(abspath $(CURDIR)/../../../kernel)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/kernel-defaults.mk

define KernelPackage/opendectcell-lantiq
  SUBMENU:=Other modules
  TITLE:=FRITZ!Box 7412 DECT chip control and PCM/TDM
  DEPENDS:=@TARGET_lantiq
  FILES:=$(PKG_BUILD_DIR)/opendectcell.ko
  # Deliberately no AUTOLOAD: only manual insmod is permitted.
endef

# Only the Lantiq (VR9 SSC) transport sources are copied into the build.
define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/include/uapi
	$(CP) $(OPENDECTCELL_KERNEL_SRC)/opendectcell_lantiq.c \
		$(OPENDECTCELL_KERNEL_SRC)/opendectcell_ssc.c \
		$(OPENDECTCELL_KERNEL_SRC)/opendectcell_ssc.h \
		$(OPENDECTCELL_KERNEL_SRC)/Makefile $(PKG_BUILD_DIR)/
	$(CP) $(OPENDECTCELL_KERNEL_SRC)/include/uapi/opendectcell.h \
		$(PKG_BUILD_DIR)/include/uapi/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" OPENDECTCELL_LANTIQ=1 modules
endef

$(eval $(call KernelPackage,opendectcell-lantiq))
