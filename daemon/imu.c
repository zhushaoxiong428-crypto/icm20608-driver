/*
 * ICM20608 IIO 缓冲区读取（驱动：icm20608-driver/）
 *
 * 内核：数据就绪中断 -> 上半部记时间戳 -> 线程化下半部 SPI 读 14 字节 -> 推入 kfifo。
 * 这里：打开全部扫描通道、设置采样率/缓冲区长度/watermark 并启用缓冲区；
 *       调用方定时（100 ms）调用 imu_drain() 非阻塞读走全部记录。
 *       不用 poll：4.1 的 iio_push_to_buffer() 每推入一个样本都唤醒 poll 等待者，
 *       实测 100 Hz 时每秒唤醒约 126 次，定时读约 10 次。
 * 记录布局按 scan_elements/<通道>_index、_type 计算，不写死 24 字节结构。
 */
#include "devmond.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const imu_names[] = { "icm20608d", "icm20608", NULL };

static const char *const field_names[IMU_FIELDS] = {
	[IMU_AX] = "in_accel_x", [IMU_AY] = "in_accel_y", [IMU_AZ] = "in_accel_z",
	[IMU_TEMP] = "in_temp",
	[IMU_GX] = "in_anglvel_x", [IMU_GY] = "in_anglvel_y", [IMU_GZ] = "in_anglvel_z",
	[IMU_TS] = "in_timestamp",
};

static int cmp_index(const void *a, const void *b)
{
	return ((const struct scan_elem *)a)->index - ((const struct scan_elem *)b)->index;
}

/* 遍历 scan_elements 下的 xxx_en：enable 为真时写 1，否则只收集已打开的通道 */
static int scan_elements(struct imu *imu, bool enable, char *err)
{
	char scan_dir[200];
	struct dirent *e;
	DIR *d;
	int ret = 0;

	snprintf(scan_dir, sizeof(scan_dir), "%s/scan_elements", imu->dir);
	d = opendir(scan_dir);
	if (!d) {
		snprintf(err, ERRLEN, "open %s: %s", scan_dir, strerror(errno));
		return -1;
	}

	imu->nelems = 0;
	while ((e = readdir(d)) != NULL && !ret) {
		size_t len = strlen(e->d_name);
		struct scan_elem *el;
		char attr[96], val[48], e1, e2, sign;

		if (len < 4 || strcmp(e->d_name + len - 3, "_en"))
			continue;
		snprintf(attr, sizeof(attr), "scan_elements/%s", e->d_name);

		if (enable) {
			if (iio_write_attr(imu->dir, attr, "1")) {
				snprintf(err, ERRLEN, "enable %s failed", e->d_name);
				ret = -1;
			}
			continue;
		}

		if (iio_read_attr(imu->dir, attr, val, sizeof(val)) || strcmp(val, "1"))
			continue;
		if (imu->nelems >= (int)(sizeof(imu->elems) / sizeof(imu->elems[0])))
			break;

		el = &imu->elems[imu->nelems];
		snprintf(el->name, sizeof(el->name), "%.*s", (int)(len - 3), e->d_name);

		snprintf(attr, sizeof(attr), "scan_elements/%s_index", el->name);
		if (iio_read_attr(imu->dir, attr, val, sizeof(val)))
			continue;
		el->index = atoi(val);

		/* 例如 "be:s16/16>>0"：字节序:符号 有效位/存储位>>移位 */
		snprintf(attr, sizeof(attr), "scan_elements/%s_type", el->name);
		if (iio_read_attr(imu->dir, attr, val, sizeof(val))
		    || sscanf(val, "%c%c:%c%d/%d>>%d", &e1, &e2, &sign, &el->realbits,
			      &el->bytes, &el->shift) != 6
		    || el->bytes % 8 || el->bytes == 0) {
			snprintf(err, ERRLEN, "unsupported scan type %s = \"%s\"", el->name, val);
			ret = -1;
			break;
		}
		el->big_endian = e1 == 'b';
		el->is_signed = sign == 's';
		el->bytes /= 8;
		imu->nelems++;
	}
	closedir(d);
	return ret;
}

/* 按 index 排序，每个通道按自身字节数对齐，整条记录按最大字节数对齐（本驱动 24 字节） */
static int compute_layout(struct imu *imu, char *err)
{
	int offset = 0, maxbytes = 1, i, f;

	qsort(imu->elems, imu->nelems, sizeof(imu->elems[0]), cmp_index);
	for (i = 0; i < imu->nelems; i++) {
		struct scan_elem *el = &imu->elems[i];

		if (offset % el->bytes)
			offset += el->bytes - offset % el->bytes;
		el->offset = offset;
		offset += el->bytes;
		if (el->bytes > maxbytes)
			maxbytes = el->bytes;
	}
	imu->record_size = (offset % maxbytes) ? (offset + maxbytes - offset % maxbytes) : offset;

	for (f = 0; f < IMU_FIELDS; f++) {
		imu->field[f] = -1;
		for (i = 0; i < imu->nelems; i++)
			if (!strcmp(imu->elems[i].name, field_names[f]))
				imu->field[f] = i;
		if (imu->field[f] < 0 && f != IMU_TS) {
			snprintf(err, ERRLEN, "scan layout lacks %s", field_names[f]);
			return -1;
		}
	}
	return imu->record_size > 0 ? 0 : -1;
}

/* 字节序 -> 右移 -> 截取有效位 -> 符号扩展 */
static int64_t extract(const uint8_t *rec, const struct scan_elem *el)
{
	uint64_t raw = 0;
	int i;

	for (i = 0; i < el->bytes; i++)
		raw = (raw << 8) | rec[el->offset + (el->big_endian ? i : el->bytes - 1 - i)];
	raw >>= el->shift;
	if (el->realbits < 64) {
		uint64_t mask = (1ULL << el->realbits) - 1;

		raw &= mask;
		if (el->is_signed && (raw & (1ULL << (el->realbits - 1))))
			raw |= ~mask;
	}
	return (int64_t)raw;
}

int imu_open(struct imu *imu, int rate_hz, int drains_per_sec, char *err)
{
	char val[32];
	int watermark, length;

	memset(imu, 0, sizeof(*imu));
	imu->fd = -1;

	if (iio_find_device(imu_names, imu->dir, sizeof(imu->dir),
			    imu->node, sizeof(imu->node))) {
		snprintf(err, ERRLEN, "ICM20608 IIO device not found (is icm20608.ko loaded?)");
		return -1;
	}

	/* 缓冲区开着时不能改通道和采样率；上次异常退出可能没关 */
	if (iio_write_attr(imu->dir, "buffer/enable", "0")) {
		snprintf(err, ERRLEN, "disable IIO buffer failed");
		return -1;
	}

	/* IIO 约定：物理量 = (raw + offset) * scale */
	if (iio_read_double(imu->dir, "in_accel_scale", &imu->accel_scale)
	    || iio_read_double(imu->dir, "in_anglvel_scale", &imu->gyro_scale)
	    || iio_read_double(imu->dir, "in_temp_scale", &imu->temp_scale)
	    || iio_read_double(imu->dir, "in_temp_offset", &imu->temp_offset)) {
		snprintf(err, ERRLEN, "read IIO scale/offset failed");
		return -1;
	}

	/* 驱动的 available_scan_masks 只允许全部通道一起打开 */
	if (scan_elements(imu, true, err))
		return -1;

	snprintf(val, sizeof(val), "%d", rate_hz);
	if (iio_write_attr(imu->dir, "sampling_frequency", val)) {
		snprintf(err, ERRLEN, "set sampling_frequency %d failed", rate_hz);
		return -1;
	}
	/* 芯片按分频取整，以回读为准 */
	imu->rate_hz = iio_read_attr(imu->dir, "sampling_frequency", val, sizeof(val)) ?
		       rate_hz : atoi(val);

	if (scan_elements(imu, false, err) || compute_layout(imu, err))
		return -1;

	watermark = imu->rate_hz / (drains_per_sec > 0 ? drains_per_sec : 1);
	if (watermark < 1)
		watermark = 1;
	length = imu->rate_hz * 4 > watermark * 4 ? imu->rate_hz * 4 : watermark * 4;

	snprintf(val, sizeof(val), "%d", length);
	if (iio_write_attr(imu->dir, "buffer/length", val)) {
		snprintf(err, ERRLEN, "set buffer/length failed");
		return -1;
	}
	snprintf(val, sizeof(val), "%d", watermark);
	iio_write_attr(imu->dir, "buffer/watermark", val);	/* 老内核没有，不算错 */
	if (iio_write_attr(imu->dir, "buffer/enable", "1")) {
		snprintf(err, ERRLEN, "enable IIO buffer failed");
		return -1;
	}

	imu->fd = open(imu->node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (imu->fd < 0) {
		snprintf(err, ERRLEN, "open %s: %s", imu->node, strerror(errno));
		iio_write_attr(imu->dir, "buffer/enable", "0");
		return -1;
	}

	imu->buflen = (size_t)imu->record_size * (watermark * 2 > 64 ? watermark * 2 : 64);
	imu->buf = malloc(imu->buflen);
	if (!imu->buf) {
		snprintf(err, ERRLEN, "out of memory");
		imu_close(imu);
		return -1;
	}
	return 0;
}

void imu_close(struct imu *imu)
{
	if (imu->fd >= 0) {
		close(imu->fd);
		imu->fd = -1;
	}
	/* 关掉缓冲区，芯片停止产生数据就绪中断 */
	if (imu->dir[0])
		iio_write_attr(imu->dir, "buffer/enable", "0");
	free(imu->buf);
	imu->buf = NULL;
}

int imu_drain(struct imu *imu, imu_sample_cb cb, void *ctx, char *err)
{
	int count = 0;

	if (imu->fd < 0) {
		snprintf(err, ERRLEN, "ICM20608 IIO buffer not open");
		return -1;
	}

	for (;;) {
		ssize_t n = read(imu->fd, imu->buf, imu->buflen);
		ssize_t pos;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			snprintf(err, ERRLEN, "read %s: %s", imu->node, strerror(errno));
			return -1;
		}
		if (n == 0)
			break;

		for (pos = 0; pos + imu->record_size <= n; pos += imu->record_size) {
			const uint8_t *rec = imu->buf + pos;
			struct imu_sample s;
			int axis;

			for (axis = 0; axis < 3; axis++) {
				s.accel[axis] = extract(rec, &imu->elems[imu->field[IMU_AX + axis]])
						* imu->accel_scale;
				s.gyro[axis] = extract(rec, &imu->elems[imu->field[IMU_GX + axis]])
					       * imu->gyro_scale;
			}
			/* 温度单位为毫摄氏度 */
			s.temp_c = (extract(rec, &imu->elems[imu->field[IMU_TEMP]]) + imu->temp_offset)
				   * imu->temp_scale / 1000.0;
			s.ts_ns = imu->field[IMU_TS] >= 0 ?
				  extract(rec, &imu->elems[imu->field[IMU_TS]]) : 0;
			cb(&s, ctx);
			count++;
		}

		if ((size_t)n < imu->buflen)	/* 没读满说明已读空 */
			break;
	}
	return count;
}
