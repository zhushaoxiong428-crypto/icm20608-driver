/*
 * v4 对照组：不写驱动，用户态通过 /dev/spidev2.0 直接访问 ICM-20608，
 * 按固定周期定时读取（很多人用传感器就是这么做的）。
 *
 * 用法：./spidev_poll [采样率Hz] [秒数] [other|fifo]
 *       默认 1000 Hz、10 秒、普通调度；fifo = SCHED_FIFO 优先级 50 + mlockall
 *
 * 统计：
 *   - 两次读之间的间隔（均值 / 标准差 / p99 / 最大）
 *   - 唤醒延迟：实际醒来时间 - 计划时间
 *   - 重复样本：连续两次读到完全相同的 14 字节，说明芯片还没出新数据
 *   - 漏掉的样本：芯片在这段时间产生的样本数 - 读到的不同样本数
 *     （用户态定时器和芯片时钟不同步，必然会重复和漏读）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/spi/spidev.h>

#include "cpustat.h"

#define DEV		"/dev/spidev2.0"
#define SPEED_HZ	1000000

static int fd;

static int xfer(uint8_t *tx, uint8_t *rx, int len)
{
	struct spi_ioc_transfer t;

	memset(&t, 0, sizeof(t));
	t.tx_buf = (unsigned long)tx;
	t.rx_buf = (unsigned long)rx;
	t.len = len;
	t.speed_hz = SPEED_HZ;
	t.bits_per_word = 8;
	return ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0 ? -1 : 0;
}

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { reg & 0x7F, val }, rx[2];

	return xfer(tx, rx, 2);
}

static int rd(uint8_t reg)
{
	uint8_t tx[2] = { reg | 0x80, 0 }, rx[2];

	return xfer(tx, rx, 2) ? -1 : rx[1];
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return (x > y) - (x < y);
}

static void stats(const char *name, int64_t *v, size_t m)
{
	double sum = 0, sq = 0, mean;
	size_t i;

	for (i = 0; i < m; i++)
		sum += v[i];
	mean = sum / m;
	for (i = 0; i < m; i++)
		sq += (v[i] - mean) * (v[i] - mean);
	qsort(v, m, sizeof(*v), cmp_i64);
	printf("%s us: mean %.1f  sd %.1f  min %.1f  p50 %.1f  p99 %.1f  max %.1f\n",
	       name, mean / 1e3, sqrt(sq / m) / 1e3, v[0] / 1e3, v[m / 2] / 1e3,
	       v[(size_t)(m * 0.99)] / 1e3, v[m - 1] / 1e3);
}

int main(int argc, char *argv[])
{
	static const uint8_t init[][2] = {
		{ 0x6A, 0x10 },	/* USER_CTRL: I2C_IF_DIS */
		{ 0x6C, 0x00 },	/* PWR_MGMT_2: 全部轴打开 */
		{ 0x19, 0x00 },	/* SMPLRT_DIV: 1 kHz */
		{ 0x1A, 0x03 },	/* CONFIG: DLPF */
		{ 0x1B, 0x18 },	/* GYRO_CONFIG: +-2000 dps */
		{ 0x1C, 0x00 },	/* ACCEL_CONFIG: +-2 g */
		{ 0x1D, 0x03 },	/* ACCEL_CONFIG2 */
		{ 0x38, 0x00 },	/* INT_ENABLE: 不用中断 */
	};
	int rate = argc > 1 ? atoi(argv[1]) : 1000;
	double secs = argc > 2 ? atof(argv[2]) : 10.0;
	int fifo = argc > 3 && !strcmp(argv[3], "fifo");
	int64_t period = 1000000000LL / rate;
	size_t cap = (size_t)(rate * secs) + 16, n = 0, stale = 0, overrun = 0, i;
	int64_t *t_read = malloc(cap * sizeof(int64_t));
	int64_t *lat = malloc(cap * sizeof(int64_t));
	uint8_t tx[15] = { 0x3B | 0x80 }, rx[15], prev[14];
	struct cpustat c0, c1;
	struct timespec next;
	uint8_t mode = SPI_MODE_0;
	int64_t t_next, t0;

	fd = open(DEV, O_RDWR);
	if (fd < 0 || !t_read || !lat) {
		perror(DEV);
		return 1;
	}
	ioctl(fd, SPI_IOC_WR_MODE, &mode);

	/* 芯片初始化：与驱动相同的顺序（复位 -> 唤醒 -> 配置） */
	if (rd(0x75) != 0xAE && rd(0x75) != 0xAF) {
		fprintf(stderr, "WHO_AM_I mismatch: 0x%02x\n", rd(0x75));
		return 1;
	}
	wr(0x6B, 0x80);
	usleep(100000);
	wr(0x6B, 0x01);
	usleep(10000);
	for (i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
		wr(init[i][0], init[i][1]);
		if (rd(init[i][0]) != init[i][1]) {
			fprintf(stderr, "init reg 0x%02x failed\n", init[i][0]);
			return 1;
		}
	}
	usleep(50000);

	if (fifo) {
		struct sched_param sp = { .sched_priority = 50 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp) || mlockall(MCL_CURRENT | MCL_FUTURE))
			perror("SCHED_FIFO/mlockall");
	}
	printf("spidev poll: %d Hz, %.1f s, %s\n", rate, secs, fifo ? "SCHED_FIFO 50" : "SCHED_OTHER");

	cpustat_read(&c0);
	memset(prev, 0, sizeof(prev));
	t0 = t_next = now_ns();
	while (n < cap && t_next - t0 < (int64_t)(secs * 1e9)) {
		int64_t woke;

		t_next += period;
		next.tv_sec = t_next / 1000000000LL;
		next.tv_nsec = t_next % 1000000000LL;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		woke = now_ns();
		lat[n] = woke - t_next;
		if (lat[n] > period)
			overrun++;

		if (xfer(tx, rx, sizeof(rx))) {
			perror("SPI_IOC_MESSAGE");
			break;
		}
		t_read[n] = now_ns();
		if (n && !memcmp(prev, rx + 1, 14))
			stale++;
		memcpy(prev, rx + 1, 14);
		n++;
	}
	cpustat_read(&c1);
	wr(0x6B, 0x40);	/* 芯片睡眠 */

	{
		size_t m = n - 1;
		int64_t *dt = malloc(m * sizeof(int64_t));
		double span = (t_read[n - 1] - t_read[0]) / 1e9;
		/* 芯片实际频率（v3 实测 1000.9 Hz）按标称算；漏读 = 应有样本 - 不同样本 */
		long produced = (long)(span * rate + 0.5) + 1;
		long unique = (long)(n - stale);

		for (i = 0; i < m; i++)
			dt[i] = t_read[i + 1] - t_read[i];
		printf("reads %zu in %.3f s -> %.1f Hz\n", n, span, m / span);
		stats("interval", dt, m);
		stats("wakeup latency", lat, n);
		printf("overruns (woke > 1 period late): %zu\n", overrun);
		printf("stale (same sample read twice): %zu (%.2f%%)\n", stale, 100.0 * stale / n);
		printf("missed chip samples (approx): %ld (%.2f%%)\n",
		       produced > unique ? produced - unique : 0,
		       produced > unique ? 100.0 * (produced - unique) / produced : 0.0);
		printf("system CPU busy: %.1f%%\n", cpustat_busy_pct(&c0, &c1));
		free(dt);
	}
	close(fd);
	return 0;
}
