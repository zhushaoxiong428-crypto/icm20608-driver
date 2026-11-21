#include "devmond.h"

#include <stdarg.h>
#include <time.h>

int64_t realtime_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void format_iso_ms(int64_t ns, char *buf, size_t len)
{
	time_t sec = (time_t)(ns / 1000000000LL);
	int ms = (int)((ns % 1000000000LL) / 1000000);
	struct tm tm;
	size_t n;

	localtime_r(&sec, &tm);
	n = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm);
	snprintf(buf + n, len - n, ".%03d", ms);
}

/* 日志写 stderr；守护进程模式下 stderr 被重定向到日志文件 */
void dlog(const char *fmt, ...)
{
	char stamp[32];
	va_list ap;

	format_iso_ms(realtime_ns(), stamp, sizeof(stamp));
	fprintf(stderr, "%s devmond: ", stamp);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}
