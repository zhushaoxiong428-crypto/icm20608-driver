KERNELDIR := $(HOME)/alpha/src/linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek

obj-m := icm20608.o

all:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean

