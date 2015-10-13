#
#This software is contributed or developed by KYOCERA Corporation.
#(C) 2012 KYOCERA Corporation
#
#
# Makefile for the Silicon Image 8334 MHL TX device driver
#
# example invocations:	
#	make ARCH=arm CROSS_COMPILE=arm-angstrom-linux-gnueabi- sii8334_clean sii8334_debug
#	make ARCH=arm CROSS_COMPILE=arm-angstrom-linux-gnueabi- sii8334_clean sii8334_release
#	make ARCH=arm CROSS_COMPILE=arm-angstrom-linux-gnueabi- sii8334_clean sii8334_clean

#export ANDROID_HOME=$(ANDROID)
#KERNEL_SRC = $(ANDROID_HOME)/out/debug/target/product/msm8960/obj/KERNEL_OBJ/
#KERNEL_SRC = $(ANDROID_HOME)/kernel/
#KERNEL_SRC = $(ANDROID_HOME)/out/target/product/msm8960/obj/KERNEL_OBJ/

#BUILD_DIR := $(shell pwd)
#CROSS_COMPILE = $(ANDROID_HOME)/prebuilt/linux-x86/toolchain/arm-eabi-4.4.0/bin/arm-eabi-
#ARCH = arm
#BUILD_DIR := $(shell pwd)

ccflags-y := -I$(src)/include
ccflags-y += -I$(src)/platform
ccflags-y += -I$(src)/platform/hal
ccflags-y += -I$(src)/component/mhl_tx
ccflags-y += -I$(src)/driver/cra_drv
ccflags-y += -I$(src)/driver/sii8334_drv
ccflags-y += -I$(src)/linux_kernel/kernel_module
ccflags-y += -I$(src)/platform/api
ccflags-y += -I$(src)/platform/include
ccflags-y += -DMAKE_8334_DRIVER
ccflags-y += -DASSERT_PUSH_PULL
ccflags-y += -DC99_VA_ARG_SUPPORT
ccflags-y += -DENABLE_TX_DEBUG_PRINT
ccflags-y += -DENABLE_CBUS_DEBUG_PRINT
ccflags-y += -DBUILDNUM=$(BUILDNUM)
ccflags-y += -DDEBUG


#
# Module Name
# output file name is $(MODULES).ko
#

MODULES = sii8334drv


obj-y := $(MODULES).o


#----------------- App layer --------------------------------------
$(MODULES)-objs := \
                   linux_kernel/kernel_module/mhl_linuxdrv_main.o \
                   linux_kernel/public/mhl_linuxdrv_ioctl.o \
                   linux_kernel/public/mhl_linuxdrv_open_close.o
#----------------- Component layer --------------------------------
$(MODULES)-objs += component/mhl_tx/si_mhl_tx.o \
                   component/mhl_tx/si_mhl_tx_rcp_table.o
#----------------- Platform layer ---------------------------------
$(MODULES)-objs += platform/osal/si_osdebug.o \
                   platform/osal/sii_osal_linux_timer.o \
                   platform/hal/sii_hal_linux_init.o \
                   platform/hal/sii_hal_linux_i2c.o \
                   platform/hal/sii_hal_linux_isr.o \
                   platform/hal/sii_hal_linux_timer.o \
                   platform/hal/sii_hal_linux_gpio.o
#----------------- Driver layer --------------------------------
$(MODULES)-objs += driver/cra_drv/si_cra.o \
                   driver/sii8334_drv/si_cra_cfg.o \
                   driver/sii8334_drv/si_drv_mhl_tx.o


#MAKEARCH = make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

#all:
#	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_SRC) SUBDIRS=$(BUILD_DIR) CONFIG_DEBUG_DRIVER=y modules

#release:
#	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)strip $(KERNEL_SRC) SUBDIRS=$(BUILD_DIR) CONFIG_DEBUG_DRIVER=y modules





clean:
	rm -f .*.o.*
	rm -f .*.ko.*
	rm -f *.o
	rm -f *.ko
	rm -f *.mod.c
	rm -f *.mod.o
	rm -f component/mhl_tx/*.o
	rm -f component/mhl_tx/.*.o.*
	rm -f driver/cra_drv/*.o
	rm -f driver/cra_drv/.*.o.*
	rm -f driver/sii8334_drv/*.o
	rm -f driver/sii8334_drv/.*.o.*
	rm -f linux_kernel/kernel_module/*.o
	rm -f linux_kernel/kernel_module/.*.o.*
	rm -f linux_kernel/public/*.o
	rm -f linux_kernel/public/.*.o.*
	rm -f platform/hal/*.o
	rm -f platform/hal/.*.o.*
	rm -f platform/osal/*.o
	rm -f platform/osal/.*.o.*
	rm -f Module.symvers
	rm -f modules.order
	rm -rf .tmp_versions
	
	
