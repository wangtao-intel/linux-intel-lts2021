ifneq ($(KERNELRELEASE),)
obj-$(CONFIG_TOUCHSCREEN_EF1E_TP) += ef1e_tp.o
ef1e_tp-objs += ef1e_tp_input.o \
	ef1e_tp_driver.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build

ccflags-y += -DDEBUG
ccflags-m += -DDEBUG

default:
	$(MAKE) -C $(KDIR) M=$$PWD CONFIG_TOUCHSCREEN_EF1E_TP=m modules

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
endif
