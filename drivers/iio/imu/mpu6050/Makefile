# SPDX-License-Identifier: GPL-2.0-only
#
# Dual-purpose Makefile: builds in-tree (kbuild) and as an external module.
#
# External build (cross-compile for STM32MP157 on the Ubuntu VM):
#   make KDIR=/path/to/mp157-kernel \
#        ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
#
# Native build against the running kernel:
#   make
#
# Produces two modules, mirroring the upstream inv_mpu6050 split:
#   mpu6050-core.ko  device model, channels, PM, buffer, events, self-test
#   mpu6050-i2c.ko   I2C bus front-end (module entry point)

ifneq ($(KERNELRELEASE),)

obj-m += mpu6050-core.o
obj-m += mpu6050-i2c.o

mpu6050-core-y := mpu6050_core.o mpu6050_buffer.o \
		  mpu6050_events.o mpu6050_selftest.o
mpu6050-i2c-y  := mpu6050_i2c.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all modules_install clean

endif
