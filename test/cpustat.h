/* 读 /proc/stat 的总 cpu 行，计算一段时间内整机 CPU 忙碌比例 */
#ifndef CPUSTAT_H
#define CPUSTAT_H

#include <stdio.h>

struct cpustat {
	unsigned long long busy, total;
};

static inline void cpustat_read(struct cpustat *s)
{
	unsigned long long v[8] = {0};
	FILE *f = fopen("/proc/stat", "r");

	s->busy = s->total = 0;
	if (!f)
		return;
	/* user nice system idle iowait irq softirq steal */
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
		int i;

		for (i = 0; i < 8; i++)
			s->total += v[i];
		s->busy = s->total - v[3] - v[4];
	}
	fclose(f);
}

static inline double cpustat_busy_pct(const struct cpustat *a,
				      const struct cpustat *b)
{
	unsigned long long dt = b->total - a->total;

	return dt ? 100.0 * (b->busy - a->busy) / dt : 0;
}

#endif
