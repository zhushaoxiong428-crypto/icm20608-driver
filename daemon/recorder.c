/*
 * CSV 录制（文件名与列和原 Qt 版一致）：
 *   records/ap3216c_YYYYMMDD_HHMMSS_mmm.csv   每个采集周期一行
 *   records/icm20608_YYYYMMDD_HHMMSS_mmm.csv  每个 IIO 样本一行，时间取内核时间戳
 * 守护进程负责录制，界面退出后录制继续
 */
#include "devmond.h"
#include "ap3216c_data.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const double kStandardGravity = 9.80665;
static const double kRadToDeg = 57.29577951308232;

/* O_EXCL：已有同名文件时失败，不覆盖旧记录 */
static FILE *create_new(const char *path, char *err)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	FILE *f;

	if (fd < 0 || !(f = fdopen(fd, "w"))) {
		snprintf(err, ERRLEN, "create %s: %s", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	return f;
}

int recorder_start(struct recorder *r, const char *dir, char *err)
{
	char stamp[40], path[300];
	int64_t now = realtime_ns();
	time_t sec = (time_t)(now / 1000000000LL);
	struct tm tm;
	size_t n;

	recorder_stop(r);
	snprintf(r->dir, sizeof(r->dir), "%s", dir);
	if (mkdir(dir, 0755) && errno != EEXIST) {
		snprintf(err, ERRLEN, "create %s: %s", dir, strerror(errno));
		return -1;
	}

	localtime_r(&sec, &tm);
	n = strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
	snprintf(stamp + n, sizeof(stamp) - n, "_%03d", (int)(now % 1000000000LL / 1000000));

	snprintf(path, sizeof(path), "%s/ap3216c_%s.csv", dir, stamp);
	r->ap = create_new(path, err);
	if (!r->ap)
		return -1;
	snprintf(path, sizeof(path), "%s/icm20608_%s.csv", dir, stamp);
	r->imu = create_new(path, err);
	if (!r->imu) {
		fclose(r->ap);
		r->ap = NULL;
		return -1;
	}

	fputs("timestamp,ir_raw,als_raw,als_lux,ps_raw,flags\n", r->ap);
	fputs("timestamp,kernel_timestamp_ns,accel_x_g,accel_y_g,accel_z_g,"
	      "gyro_x_dps,gyro_y_dps,gyro_z_dps,temp_c\n", r->imu);
	if (recorder_flush(r, err)) {
		recorder_stop(r);
		return -1;
	}

	/* IMU 样本按 100 ms 批量到达，跳过开始之前采到的 */
	r->start_ns = now;
	r->on = true;
	return 0;
}

void recorder_stop(struct recorder *r)
{
	if (r->ap)
		fclose(r->ap);
	if (r->imu)
		fclose(r->imu);
	r->ap = r->imu = NULL;
	r->on = false;
}

int recorder_ap(struct recorder *r, int64_t ts_ns, int ir, int als, double lux,
		int ps, int flags)
{
	char t[32];

	if (!r->on)
		return 0;
	format_iso_ms(ts_ns, t, sizeof(t));
	fprintf(r->ap, "%s,", t);
	if (flags & AP3216C_IR_VALID)
		fprintf(r->ap, "%d", ir);
	fputc(',', r->ap);
	if (flags & AP3216C_ALS_VALID)
		fprintf(r->ap, "%d,%.2f", als, lux);
	else
		fputc(',', r->ap);
	fputc(',', r->ap);
	if (flags & AP3216C_PS_VALID)
		fprintf(r->ap, "%d", ps);
	fprintf(r->ap, ",%d\n", flags);
	return 0;
}

int recorder_imu(struct recorder *r, const struct imu_sample *s)
{
	char t[32];

	if (!r->on || s->ts_ns < r->start_ns)
		return 0;
	format_iso_ms(s->ts_ns, t, sizeof(t));
	fprintf(r->imu, "%s,%lld,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.2f\n", t, (long long)s->ts_ns,
		s->accel[0] / kStandardGravity, s->accel[1] / kStandardGravity,
		s->accel[2] / kStandardGravity, s->gyro[0] * kRadToDeg,
		s->gyro[1] * kRadToDeg, s->gyro[2] * kRadToDeg, s->temp_c);
	return 0;
}

/* stdio 缓冲攒一批再写，flush 时检查错误（磁盘满等） */
int recorder_flush(struct recorder *r, char *err)
{
	if (!r->ap || !r->imu)
		return 0;
	if (fflush(r->ap) || fflush(r->imu) || ferror(r->ap) || ferror(r->imu)) {
		snprintf(err, ERRLEN, "write CSV failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}
