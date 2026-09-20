obj-m += src/cpu_oc_mt6789.o
obj-m += src/cpu_oc_diag_mt6789.o
obj-m += src/cpu_oc_noop_mt6789.o

KDIR ?= /lib/modules/$(shell uname -r)/build
ifeq ($(wildcard $(KDIR)/Module.symvers),)
KBUILD_MODPOST_WARN ?= 1
endif
ARCH ?= $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')
LLVM ?= 0
CROSS_COMPILE ?=

all:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) $(if $(filter 1,$(LLVM)),LLVM=1) $(if $(CROSS_COMPILE),CROSS_COMPILE=$(CROSS_COMPILE)) $(if $(KBUILD_MODPOST_WARN),KBUILD_MODPOST_WARN=$(KBUILD_MODPOST_WARN)) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) M=$(PWD) clean
