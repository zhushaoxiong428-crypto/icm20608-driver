/*
 * 用 IIO 缓冲区连续采集 ICM-20608，并按样本时间戳统计采样间隔。
 *
 * 用法：./iio_buffer [采样率Hz] [秒数] [打印前几个样本]
 *       默认 1000 Hz、5 秒、打印 3 个
 *
 * 每条记录 24 字节（驱动里 scan 的布局）：
 *   7 个 int16 大端（accel xyz, temp, gyro xyz） + 2 字节填充 + int64 时间戳(ns)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#define REC_SIZE	24
#define BATCH		64

static char dev_dir[256];

static int sysfs_write(const char *attr, const char *val)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dev_dir, attr);
	f = fopen(path, "w");
	if (!f) {
		perror(path);
		return -1;
	}
	fprintf(f, "%s", val);
	return fclose(f);
}

static double sysfs_read_double(const char *attr)
{
	char path[512];
	double v = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dev_dir, attr);
	f = fopen(path, "r");
	if (!f || fscanf(f, "%lf", &v) != 1)
		perror(path);
	if (f)
		fclose(f);
	return v;
}

/* 在 /sys/bus/iio/devices 下找 name 以 icm20608 开头的设备，返回编号 */
static int find_device(void)
{
	const char *base = "/sys/bus/iio/devices";
	struct dirent *e;
	DIR *d = opendir(base);
	int id = -1;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char path[512], name[64] = "";
		FILE *f;

		if (strncmp(e->d_name, "iio:device", 10))
			continue;
		snprintf(path, sizeof(path), "%s/%s/name", base, e->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) && !strncmp(name, "icm20608", 8)) {
			id = atoi(e->d_name + 10);
			snprintf(dev_dir, sizeof(dev_dir), "%s/%s", base, e->d_name);
		}
		fclose(f);
	}
	closedir(d);
	return id;
}

static int16_t be16(const uint8_t *p)
{
	return (int16_t)((p[0] << 8) | p[1]);
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return (x > y) - (x < y);
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char *argv[])
{
	static const char *const chans[] = {
		"in_accel_x_en", "in_accel_y_en", "in_accel_z_en", "in_temp_en",
		"in_anglvel_x_en", "in_anglvel_y_en", "in_anglvel_z_en",
		"in_timestamp_en",
	};
	int rate = argc > 1 ? atoi(argv[1]) : 1000;
	double secs = argc > 2 ? atof(argv[2]) : 5.0;
	int print_n = argc > 3 ? atoi(argv[3]) : 3;
	size_t cap = (size_t)(rate * secs * 1.5) + 1024;
	int64_t *ts = malloc(cap * sizeof(*ts));
	uint8_t buf[REC_SIZE * BATCH];
	char attr[64], devnode[64], rate_str[16];
	double as, gs, tsc, toff, t_end;
	size_t n = 0, i;
	int id, fd;

	id = find_device();
	if (id < 0 || !ts) {
		fprintf(stderr, "icm20608 IIO device not found\n");
		return 1;
	}
	as = sysfs_read_double("in_accel_scale");
	gs = sysfs_read_double("in_anglvel_scale");
	tsc = sysfs_read_double("in_temp_scale");
	toff = sysfs_read_double("in_temp_offset");

	/* 1. 配置：先关缓冲区 -> 打开全部通道 -> 设采样率 -> 设缓冲区长度 -> 打开 */
	sysfs_write("buffer/enable", "0");
	for (i = 0; i < sizeof(chans) / sizeof(chans[0]); i++) {
		snprintf(attr, sizeof(attr), "scan_elements/%s", chans[i]);
		if (sysfs_write(attr, "1"))
			return 1;
	}
	snprintf(rate_str, sizeof(rate_str), "%d", rate);
	if (sysfs_write("sampling_frequency", rate_str))
		return 1;
	sysfs_write("buffer/length", "2048");
	if (sysfs_write("buffer/enable", "1"))
		return 1;

	snprintf(devnode, sizeof(devnode), "/dev/iio:device%d", id);
	fd = open(devnode, O_RDONLY);
	if (fd < 0) {
		perror(devnode);
		sysfs_write("buffer/enable", "0");
		return 1;
	}
	printf("%s (%s): %d Hz requested, %.1f s\n", devnode, dev_dir,
	       (int)sysfs_read_double("sampling_frequency"), secs);

	/* 2. 连续读：没有数据时 read 会阻塞，直到有新样本进缓冲区 */
	t_end = now_s() + secs;
	while (now_s() < t_end && n < cap) {
		ssize_t r = read(fd, buf, sizeof(buf));
		ssize_t k;

		if (r < 0) {
			perror("read");
			break;
		}
		for (k = 0; k + REC_SIZE <= r && n < cap; k += REC_SIZE) {
			const uint8_t *rec = buf + k;
			int64_t t;

			memcpy(&t, rec + 16, sizeof(t));
			ts[n] = t;
			if ((int)n < print_n)
				printf("  #%zu a=(%.3f %.3f %.3f) m/s2  g=(%.4f %.4f %.4f) rad/s  T=%.2f C  t=%lld\n",
				       n, be16(rec) * as, be16(rec + 2) * as, be16(rec + 4) * as,
				       be16(rec + 8) * gs, be16(rec + 10) * gs, be16(rec + 12) * gs,
				       (be16(rec + 6) + toff) * tsc / 1000.0, (long long)t);
			n++;
		}
	}
	close(fd);
	sysfs_write("buffer/enable", "0");

	/* 3. 统计相邻样本的时间间隔 */
	if (n < 3) {
		printf("too few samples: %zu\n", n);
		return 1;
	}
	{
		size_t m = n - 1, lost = 0;
		int64_t *dt = malloc(m * sizeof(*dt));
		double period = 1e9 / sysfs_read_double("sampling_frequency");
		double sum = 0, sq = 0, mean, sd, span;

		for (i = 0; i < m; i++) {
			dt[i] = ts[i + 1] - ts[i];
			sum += dt[i];
			if (dt[i] > 1.5 * period)
				lost += (size_t)(dt[i] / period + 0.5) - 1;
		}
		mean = sum / m;
		for (i = 0; i < m; i++)
			sq += (dt[i] - mean) * (dt[i] - mean);
		sd = sqrt(sq / m);
		span = (ts[n - 1] - ts[0]) / 1e9;
		qsort(dt, m, sizeof(*dt), cmp_i64);

		printf("samples %zu in %.3f s -> %.1f Hz\n", n, span, (n - 1) / span);
		printf("interval us: mean %.1f  sd %.1f  min %.1f  p50 %.1f  p99 %.1f  max %.1f\n",
		       mean / 1e3, sd / 1e3, dt[0] / 1e3, dt[m / 2] / 1e3,
		       dt[(size_t)(m * 0.99)] / 1e3, dt[m - 1] / 1e3);
		printf("gaps > 1.5 periods: estimated lost samples %zu (%.3f%%)\n",
		       lost, 100.0 * lost / (n + lost));
		free(dt);
	}
	free(ts);
	return 0;
}
