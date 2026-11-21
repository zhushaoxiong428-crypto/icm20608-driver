/*
 * 系统状态采集：全部来自 /proc 与 /sys，比率类数据用两次采样的差值计算
 *
 *   CPU 占用     /proc/stat 第一行：(总 jiffies - idle - iowait) 的增量 / 总增量
 *   devmond 自身 /proc/self/stat 的 utime + stime 增量 / (CLK_TCK * 经过秒数)
 *   内存         /proc/meminfo：MemTotal - MemAvailable（4.1 内核已有 MemAvailable）
 *   负载、运行时间 /proc/loadavg、/proc/uptime
 *   芯片温度     /sys/class/thermal/thermal_zone0/temp（imx_thermal，毫摄氏度）
 *   网络         /proc/net/dev 除 lo 外所有网卡的收发字节增量
 *   存储         statvfs(录制目录)，录制前确认空间
 */
#include "devmond.h"

#include <errno.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int read_file(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, len - 1, f);
	fclose(f);
	buf[n] = '\0';
	return 0;
}

static int read_cpu(uint64_t *total, uint64_t *idle)
{
	unsigned long long v[8] = { 0 };
	char buf[256];
	int i;

	if (read_file("/proc/stat", buf, sizeof(buf))
	    || sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		      &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4)
		return -1;
	*total = 0;
	for (i = 0; i < 8; i++)
		*total += v[i];
	*idle = v[3] + v[4];	/* idle + iowait */
	return 0;
}

/* /proc/self/stat：第 2 个字段 (comm) 可能含空格，从最后一个 ')' 之后再数 */
static int read_self_ticks(uint64_t *ticks)
{
	unsigned long long utime, stime;
	char buf[512], *p;

	if (read_file("/proc/self/stat", buf, sizeof(buf)) || !(p = strrchr(buf, ')')))
		return -1;
	/* ')' 之后依次是 state(3) ppid pgrp session tty_nr tpgid flags minflt cminflt
	 * majflt cmajflt utime(14) stime(15) */
	if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
		   &utime, &stime) != 2)
		return -1;
	*ticks = utime + stime;
	return 0;
}

static int read_net(uint64_t *rx, uint64_t *tx)
{
	char line[256];
	FILE *f = fopen("/proc/net/dev", "r");

	if (!f)
		return -1;
	*rx = *tx = 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long r, t;
		char *colon = strchr(line, ':'), *name = line;

		if (!colon)
			continue;	/* 前两行是表头 */
		*colon = '\0';
		while (*name == ' ')
			name++;
		if (!strcmp(name, "lo"))
			continue;
		/* 接收：bytes packets errs drop fifo frame compressed multicast；发送：bytes ... */
		if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &t) == 2) {
			*rx += r;
			*tx += t;
		}
	}
	fclose(f);
	return 0;
}

static long meminfo_kb(const char *buf, const char *key)
{
	const char *p = strstr(buf, key);

	return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

void sysmon_init(struct sysmon *m)
{
	memset(m, 0, sizeof(*m));
	m->clk_tck = sysconf(_SC_CLK_TCK);
	if (m->clk_tck <= 0)
		m->clk_tck = 100;
	/* 先采一次作基准，第一次 sysmon_sample() 就能算出比率 */
	read_cpu(&m->cpu_total, &m->cpu_idle);
	read_net(&m->rx_bytes, &m->tx_bytes);
	m->last_ns = monotonic_ns();
	if (!read_self_ticks(&m->self_ticks[0])) {
		m->self_ns[0] = m->last_ns;
		m->self_count = 1;
	}
}

void sysmon_sample(struct sysmon *m, const char *disk_path, struct sysmon_sample *s)
{
	uint64_t total, idle, ticks, rx, tx;
	int64_t now = monotonic_ns();
	double dt = (now - m->last_ns) / 1e9;
	char buf[1024];
	struct statvfs vfs;

	memset(s, 0, sizeof(*s));
	if (dt <= 0)
		dt = 1e-3;

	if (!read_cpu(&total, &idle)) {
		if (total > m->cpu_total)
			s->cpu_pct = 100.0 * ((total - m->cpu_total) - (idle - m->cpu_idle))
				     / (double)(total - m->cpu_total);
		m->cpu_total = total;
		m->cpu_idle = idle;
	}
	if (!read_self_ticks(&ticks)) {
		/* 滑动窗口：与窗口里最早的一次比较 */
		if (m->self_count == SYSMON_SELF_WINDOW) {
			memmove(m->self_ticks, m->self_ticks + 1, sizeof(m->self_ticks[0]) * (SYSMON_SELF_WINDOW - 1));
			memmove(m->self_ns, m->self_ns + 1, sizeof(m->self_ns[0]) * (SYSMON_SELF_WINDOW - 1));
			m->self_count--;
		}
		m->self_ticks[m->self_count] = ticks;
		m->self_ns[m->self_count] = now;
		m->self_count++;
		if (m->self_count > 1 && now > m->self_ns[0])
			s->self_cpu_pct = 100.0 * (ticks - m->self_ticks[0])
					  / (m->clk_tck * ((now - m->self_ns[0]) / 1e9));
	}
	if (!read_net(&rx, &tx)) {
		s->rx_kbps = (rx - m->rx_bytes) / dt / 1024.0;
		s->tx_kbps = (tx - m->tx_bytes) / dt / 1024.0;
		m->rx_bytes = rx;
		m->tx_bytes = tx;
	}
	m->last_ns = now;

	if (!read_file("/proc/meminfo", buf, sizeof(buf))) {
		long total_kb = meminfo_kb(buf, "MemTotal:");
		long avail_kb = meminfo_kb(buf, "MemAvailable:");

		if (avail_kb < 0)	/* 老内核没有 MemAvailable */
			avail_kb = meminfo_kb(buf, "MemFree:") + meminfo_kb(buf, "Cached:");
		s->mem_total_mb = total_kb / 1024.0;
		s->mem_used_mb = (total_kb - avail_kb) / 1024.0;
	}
	if (!read_file("/proc/loadavg", buf, sizeof(buf)))
		s->load1 = strtod(buf, NULL);
	if (!read_file("/proc/uptime", buf, sizeof(buf)))
		s->uptime_s = strtol(buf, NULL, 10);
	if (!read_file("/sys/class/thermal/thermal_zone0/temp", buf, sizeof(buf))) {
		s->temp_c = strtol(buf, NULL, 10) / 1000.0;
		s->temp_ok = true;
	}

	/* 录制目录可能还没创建，退到它的上级目录 */
	if (statvfs(disk_path, &vfs)) {
		char parent[300];

		snprintf(parent, sizeof(parent), "%s", disk_path);
		if (statvfs(dirname(parent), &vfs))
			return;
	}
	s->disk_total_mb = (double)vfs.f_blocks * vfs.f_frsize / (1024.0 * 1024.0);
	s->disk_free_mb = (double)vfs.f_bavail * vfs.f_frsize / (1024.0 * 1024.0);
	s->disk_ok = true;
}
