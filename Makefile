obj-m += hid-switch2.o
obj-m += switch2-usb.o

hid-switch2-y := \
	switch2-hid.o \
	switch2-core.o \
	switch2-controller.o \
	switch2-init.o \
	switch2-protocol.o \
	switch2-input.o \
	switch2-jc1.o \
	switch2-ff.o \
	switch2-gatt.o

# Enable Force Feedback support as defined in the patch's Kconfig
ccflags-y += -DCONFIG_SWITCH2_FF

KVERSION ?= $(shell uname -r)

# Auto-detect if the kernel was built with the full LLVM toolchain (e.g. CachyOS)
KERNEL_CC := $(shell sed -n 's/^CONFIG_CC_IS_CLANG=y/clang/p' /lib/modules/$(KVERSION)/build/.config 2>/dev/null)
KERNEL_LD := $(shell sed -n 's/^CONFIG_LD_IS_LLD=y/lld/p' /lib/modules/$(KVERSION)/build/.config 2>/dev/null)
ifeq ($(KERNEL_CC)$(KERNEL_LD),clanglld)
  LLVM_FLAG := LLVM=1
else ifneq ($(KERNEL_CC),)
  LLVM_FLAG := CC=clang
endif

all:
	$(MAKE) -C /lib/modules/$(KVERSION)/build M=$(CURDIR) $(LLVM_FLAG) modules

clean:
	$(MAKE) -C /lib/modules/$(KVERSION)/build M=$(CURDIR) $(LLVM_FLAG) clean
