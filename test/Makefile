CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc
NFS_DIR ?= /srv/nfs/rootfs/root/drivers

all: icm20608_read

icm20608_read: icm20608_read.c ../driver/icm20608_uapi.h
	$(CC) -Wall -O2 -o $@ $< -lm

install: all
	cp icm20608_read $(NFS_DIR)/

clean:
	rm -f icm20608_read

.PHONY: all install clean
