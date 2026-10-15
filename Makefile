# SPDX-License-Identifier: GPL-2.0-only
ifneq ($(KERNELRELEASE),)
obj-m := snd-soc-piaoip-akm.o
snd-soc-piaoip-akm-y := piaoip_akm.o
else
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)
.PHONY: all modules overlay test clean
all: modules overlay
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) W=1 modules
overlay: piaoip-akm-v5.dtbo
piaoip-akm-v5.dtbo: piaoip-akm-v5-overlay.dts
	dtc -@ -I dts -O dtb -o $@ $<
test:
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 tests/test_config.c -o tests/test_config
	./tests/test_config
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) piaoip-akm-v5.dtbo tests/test_config
endif
