CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc
NFS_DIR ?= /srv/nfs/rootfs/root/drivers

all: iio_buffer

iio_buffer: iio_buffer.c
	$(CC) -Wall -O2 -o $@ $< -lm

install: all
	cp iio_buffer iio_read.sh $(NFS_DIR)/

clean:
	rm -f iio_buffer

.PHONY: all install clean
