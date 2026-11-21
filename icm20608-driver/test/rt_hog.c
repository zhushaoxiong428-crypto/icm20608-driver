/*
 * 人为制造读取延迟：以比驱动读取线程更高的实时优先级，每隔一段时间把 CPU 占满一段时间。
 * 驱动的中断线程（v3）和 FIFO 读取线程（v5）都是 SCHED_FIFO 50，本程序默认 60，
 * 占用期间它们都得不到运行（硬中断仍然照常响应）。
 *
 * 用法：./rt_hog <占用ms> [间隔ms=1000] [秒数=12] [优先级=60]
 * 内核默认的实时限流（sched_rt_runtime_us = 950000）允许每秒最多 950 ms 实时占用。
 */
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char *argv[])
{
	double busy = argc > 1 ? atof(argv[1]) / 1000 : 0.005;
	double period = argc > 2 ? atof(argv[2]) / 1000 : 1.0;
	double secs = argc > 3 ? atof(argv[3]) : 12;
	struct sched_param sp = { .sched_priority = argc > 4 ? atoi(argv[4]) : 60 };
	struct timespec next;
	double end, t;
	int n = 0;

	if (busy <= 0 || period <= busy) {
		fprintf(stderr, "usage: %s <busy ms> [period ms] [secs] [prio]\n", argv[0]);
		return 1;
	}
	mlockall(MCL_CURRENT | MCL_FUTURE);
	if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
		perror("sched_setscheduler");
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &next);
	end = now_s() + secs;
	while (now_s() < end) {
		/* 空转 busy 秒 */
		t = now_s() + busy;
		while (now_s() < t)
			;
		n++;
		/* 下一个周期起点（绝对时间） */
		next.tv_nsec += (long)(period * 1e9);
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	printf("rt_hog: %d bursts of %.1f ms every %.0f ms at SCHED_FIFO %d\n",
	       n, busy * 1000, period * 1000, sp.sched_priority);
	return 0;
}
