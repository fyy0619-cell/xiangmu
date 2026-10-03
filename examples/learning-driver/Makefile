# 学习版 MPU-6050 IIO 驱动 —— 外部模块 Makefile
#
# 在 VM 上对本机内核编译(用 i2c-stub 测试):
#   make
# 交叉编译给 STM32MP157 板子:把 KERNELDIR 指到板子的内核源码,并设 ARCH/CROSS_COMPILE
#   make KERNELDIR=/path/to/linux-5.4.31 ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
CURRENT_PATH := $(shell pwd)
obj-m := mpu6050_iio.o

build: kernel_modules

kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules
install:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules_install
clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean

.PHONY: build kernel_modules install clean
