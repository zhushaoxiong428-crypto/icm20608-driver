/*
 * AP3216C I2C 读取方式对比（用户态 i2c-dev，驱动绑定时也可用 I2C_RDWR）
 *
 * 1. 验证芯片的连续读行为（数据手册只给了单字节读时序）：
 *    把 ALS 阈值寄存器 0x1A~0x1D 临时写成不同的值，用不同起始地址/长度块读，与逐字节读对比；测完恢复。
 *    实测：地址只在一对寄存器（低字节、高字节）内自增，读满 2 字节后回到该对开头，
 *    所以从 0x0A 一次读 6 字节得到的是 IR,IR,IR，而不是 IR,ALS,PS。
 * 2. 对比读一组数据（IR/ALS/PS，0x0A~0x0F）的开销：
 *    A: 6 次单字节读（现有驱动的做法）
 *    B: 1 次 6 字节块读（结果错误，仅作对照）
 *    C: 3 次 2 字节块读（每个通道一次，可行方案）
 *    统计每组的耗时与主动上下文切换（等待 I2C 完成而睡眠的次数）
 *
 * 用法：./ap3216c_i2c_bench [/dev/i2c-0] [轮数]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define AP3216C_ADDR	0x1e
#define REG_IR_LOW	0x0a
#define REG_ALS_THR	0x1a

static int fd;

/* 组合格式：写寄存器地址 + 重复起始 + 读 len 字节 */
static int read_regs(uint8_t reg, uint8_t *buf, uint16_t len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = AP3216C_ADDR, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = AP3216C_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };

	return ioctl(fd, I2C_RDWR, &data) == 2 ? 0 : -1;
}

static int write_reg(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = AP3216C_ADDR, .flags = 0, .len = 2, .buf = buf };
	struct i2c_rdwr_ioctl_data data = { .msgs = &msg, .nmsgs = 1 };

	return ioctl(fd, I2C_RDWR, &data) == 1 ? 0 : -1;
}

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static long vcsw(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_nvcsw;
}

static int read_single(uint8_t *out)
{
	int i;

	for (i = 0; i < 6; i++)
		if (read_regs(REG_IR_LOW + i, &out[i], 1))
			return -1;
	return 0;
}

static int read_burst6(uint8_t *out)
{
	return read_regs(REG_IR_LOW, out, 6);
}

static int read_burst2x3(uint8_t *out)
{
	int i;

	for (i = 0; i < 3; i++)
		if (read_regs(REG_IR_LOW + 2 * i, &out[2 * i], 2))
			return -1;
	return 0;
}

static void bench(const char *name, int (*fn)(uint8_t *), int rounds)
{
	uint8_t d[6];
	long c0 = vcsw();
	double t0 = now_us();
	int i;

	for (i = 0; i < rounds; i++) {
		if (fn(d)) {
			perror(name);
			return;
		}
	}
	printf("%-28s %7.1f us/read  %5.1f ctx-switches/read   last IR=%u ALS=%u PS=%u\n",
	       name, (now_us() - t0) / rounds, (double)(vcsw() - c0) / rounds,
	       (d[1] << 2) | (d[0] & 0x03), (d[3] << 8) | d[2],
	       ((d[5] & 0x3f) << 4) | (d[4] & 0x0f));
}

int main(int argc, char *argv[])
{
	const char *dev = argc > 1 ? argv[1] : "/dev/i2c-0";
	int rounds = argc > 2 ? atoi(argv[2]) : 200;
	const uint8_t pattern[4] = { 0x11, 0x22, 0x33, 0x44 };
	uint8_t saved[4], single[4], burst[4];
	int i, ok;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	/* 1. 连续读行为 */
	if (read_regs(REG_ALS_THR, saved, 4)) {
		perror("read thresholds");
		return 1;
	}
	for (i = 0; i < 4; i++)
		write_reg(REG_ALS_THR + i, pattern[i]);
	for (i = 0; i < 4; i++)
		read_regs(REG_ALS_THR + i, &single[i], 1);
	printf("single-byte 0x1A..0x1D : %02x %02x %02x %02x (expected 11 22 33 44)\n",
	       single[0], single[1], single[2], single[3]);
	{
		static const struct { uint8_t reg; uint16_t len; } t[] = {
			{ 0x1a, 4 }, { 0x1a, 2 }, { 0x1c, 2 }, { 0x1b, 2 }, { 0x1b, 3 },
		};
		unsigned k, j;

		for (k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
			memset(burst, 0, sizeof(burst));
			read_regs(t[k].reg, burst, t[k].len);
			printf("burst from 0x%02X len %u: ", t[k].reg, t[k].len);
			for (j = 0; j < t[k].len; j++)
				printf("%02x ", burst[j]);
			printf("\n");
		}
	}
	for (i = 0; i < 4; i++)
		write_reg(REG_ALS_THR + i, saved[i]);
	(void)ok;

	{
		uint8_t d6[6], pairs[6];

		read_burst6(d6);
		read_burst2x3(pairs);
		printf("data 0x0A 6-byte burst : %02x %02x %02x %02x %02x %02x\n",
		       d6[0], d6[1], d6[2], d6[3], d6[4], d6[5]);
		printf("data 3 x 2-byte pairs  : %02x %02x %02x %02x %02x %02x\n",
		       pairs[0], pairs[1], pairs[2], pairs[3], pairs[4], pairs[5]);
	}

	/* 2. 开销对比 */
	bench("A: 6 x single-byte read", read_single, rounds);
	bench("B: 1 x 6-byte burst (WRONG)", read_burst6, rounds);
	bench("C: 3 x 2-byte burst read", read_burst2x3, rounds);

	close(fd);
	return 0;
}
