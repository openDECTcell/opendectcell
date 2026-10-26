# SPDX-License-Identifier: GPL-2.0-only
ifneq ($(KERNELRELEASE),)
obj-m += opendectcell.o
ifdef OPENDECTCELL_LANTIQ
opendectcell-y := opendectcell_lantiq.o opendectcell_ssc.o
else
opendectcell-y := opendectcell_ipq40xx.o opendectcell_pcm.o
endif
ccflags-y += -I$(src)/include/uapi
else
KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) clean
endif
