CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc
NFS_DIR ?= /srv/nfs/rootfs/root/drivers

all: iio_buffer spidev_poll rt_hog

iio_buffer: iio_buffer.c cpustat.h procstat.h
	$(CC) -Wall -O2 -o $@ $< -lm

spidev_poll: spidev_poll.c cpustat.h
	$(CC) -Wall -O2 -o $@ $< -lm

install: all
	cp iio_buffer spidev_poll rt_hog iio_read.sh load.sh $(NFS_DIR)/

clean:
	rm -f iio_buffer spidev_poll rt_hog

.PHONY: all install clean

rt_hog: rt_hog.c
	$(CC) -Wall -O2 -o $@ $<
