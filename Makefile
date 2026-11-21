# BSP 内核源码目录
KERNEL_DIR ?= /home/zero/linux/IMX6ULL/linux/original_linux

#交叉编译工具前缀
CROSS_COMPILE ?= arm-linux-gnueabihf-

#使用BSP内核中的设备树编译器
DTC := $(KERNEL_DIR)/scripts/dtc/dtc

#输出目录和设备树名称
BUILD_DIR := build
DT_NAME := imx6ull-device-management

.PHONY: all dtb modules app daemon clean

all: dtb modules app daemon

#GCC 的 -I 表示搜索目录，DTC 的 -I 表示输入格式
dtb: | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc -E -nostdinc -undef -D__DTS__ \
		-x assembler-with-cpp \
		-I$(KERNEL_DIR)/arch/arm/boot/dts \
		-I$(KERNEL_DIR)/include \
		-Idts \
		-o $(BUILD_DIR)/$(DT_NAME).dts \
		dts/$(DT_NAME).dts
	$(DTC) -I dts -O dtb \
		-o $(BUILD_DIR)/$(DT_NAME).dtb \
		$(BUILD_DIR)/$(DT_NAME).dts

$(BUILD_DIR):
	mkdir -p $@

# ICM-20608 使用自研 IIO 驱动（icm20608-driver/），与 drivers/ 下的模块分开编译
ICM_IIO_DIR := icm20608-driver

modules:
	$(MAKE) -C "$(KERNEL_DIR)" M="$(CURDIR)/drivers" \
		ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) modules
	$(MAKE) -C "$(KERNEL_DIR)" M="$(CURDIR)/$(ICM_IIO_DIR)/driver" \
		ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) modules

app: $(BUILD_DIR)/led_test $(BUILD_DIR)/key_test \
	$(BUILD_DIR)/key_led_test $(BUILD_DIR)/ap3216c_i2c_bench \
	$(BUILD_DIR)/iio_event_monitor \
	$(BUILD_DIR)/iio_buffer $(BUILD_DIR)/uart_test \
	$(BUILD_DIR)/touch_test

$(BUILD_DIR)/led_test: app/led_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/key_test: app/key_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/key_led_test: app/key_led_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

# AP3216C 连续读行为验证与读取方式开销对比（i2c-dev）
$(BUILD_DIR)/ap3216c_i2c_bench: app/ap3216c_i2c_bench.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc -O2 -Wall $< -o $@

# 内核自带的 IIO 事件监视工具（测接近事件）。工具链头文件没有 linux/iio/，
# 只复制内核的两个 uapi 头，避免与工具链其他头文件冲突；stdout 设为无缓冲，接管道/文件也立即输出
IIO_TOOLS := $(KERNEL_DIR)/tools/iio
$(BUILD_DIR)/iio_event_monitor: app/stdout_unbuffered.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/uapi/linux/iio
	cp $(KERNEL_DIR)/include/uapi/linux/iio/events.h $(KERNEL_DIR)/include/uapi/linux/iio/types.h \
		$(BUILD_DIR)/uapi/linux/iio/
	$(CROSS_COMPILE)gcc -O2 -I$(BUILD_DIR)/uapi $(IIO_TOOLS)/iio_event_monitor.c \
		$(IIO_TOOLS)/iio_utils.c $< -o $@

# IIO 缓冲区连续采集测试（来自驱动仓库）：./iio_buffer [采样率Hz] [秒数] [打印样本数]
$(BUILD_DIR)/iio_buffer: $(ICM_IIO_DIR)/test/iio_buffer.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc -O2 $< -o $@ -lm

$(BUILD_DIR)/uart_test: app/uart_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

$(BUILD_DIR)/touch_test: app/touch_test.c | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $< -o $@

# 采集守护进程与命令行客户端（纯 C，不依赖 Qt）
DAEMON_SRCS := daemon/main.c daemon/server.c daemon/imu.c daemon/ap.c daemon/io.c \
	daemon/config.c daemon/recorder.c daemon/iio.c daemon/log.c daemon/sysmon.c
DAEMON_CFLAGS := -std=gnu99 -D_GNU_SOURCE -O2 -Wall -Wextra -Iinclude -Idaemon

daemon: $(BUILD_DIR)/devmond $(BUILD_DIR)/devmonctl

$(BUILD_DIR)/devmond: $(DAEMON_SRCS) daemon/devmond.h include/ap3216c_data.h | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $(DAEMON_CFLAGS) $(DAEMON_SRCS) -o $@

$(BUILD_DIR)/devmonctl: daemon/devmonctl.c daemon/devmond.h | $(BUILD_DIR)
	$(CROSS_COMPILE)gcc $(DAEMON_CFLAGS) $< -o $@

clean:
	$(MAKE) -C "$(KERNEL_DIR)" M="$(CURDIR)/drivers" \
		ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) clean
	$(MAKE) -C "$(KERNEL_DIR)" M="$(CURDIR)/$(ICM_IIO_DIR)/driver" \
		ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -rf $(BUILD_DIR)
