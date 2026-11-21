/*
 * AP3216C 读取（驱动：drivers/ap3216c.c）
 *
 * 数值：in_intensity_ir_raw / in_illuminance_raw / in_proximity_raw，fd 打开一次，每次 pread 偏移 0；
 *       IR 溢出时 IR、PS 读返回 EAGAIN，对应通道标为无效。
 * 事件：对 /dev/iio:deviceN 做 IIO_GET_EVENT_FD_IOCTL 得到事件 fd，交给 epoll；
 *       PS 越过阈值时驱动在 AP_INT 中断里推送 rising（远到近）/ falling（近到远）。
 */
#include "devmond.h"
#include "ap3216c_data.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* 工具链头文件没有 linux/iio/，按内核 include/uapi/linux/iio/events.h、types.h 定义 */
struct iio_event_data {
	uint64_t id;
	int64_t timestamp;
};
#define IIO_GET_EVENT_FD_IOCTL	_IOR('i', 0x90, int)
#define IIO_PROXIMITY		8	/* enum iio_chan_type */
#define IIO_EV_TYPE_THRESH	0	/* enum iio_event_type */
#define IIO_EV_DIR_RISING	1	/* enum iio_event_direction */
#define IIO_EV_DIR_FALLING	2

static const char *const ap_names[] = { "ap3216c", NULL };

int ap_open(struct ap *ap, char *err)
{
	static const char *const attrs[3] = {
		"in_intensity_ir_raw", "in_illuminance_raw", "in_proximity_raw",
	};
	int *fds[3] = { &ap->ir_fd, &ap->als_fd, &ap->ps_fd };
	char node[64], path[300];
	int i;

	memset(ap, 0, sizeof(*ap));
	ap->ir_fd = ap->als_fd = ap->ps_fd = ap->event_fd = -1;

	if (iio_find_device(ap_names, ap->dir, sizeof(ap->dir), node, sizeof(node))) {
		snprintf(err, ERRLEN, "AP3216C IIO device not found (is ap3216c.ko loaded?)");
		return -1;
	}

	for (i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%s/%s", ap->dir, attrs[i]);
		*fds[i] = open(path, O_RDONLY | O_CLOEXEC);
		if (*fds[i] < 0) {
			snprintf(err, ERRLEN, "open %s: %s", path, strerror(errno));
			ap_close(ap);
			return -1;
		}
	}

	if (iio_read_double(ap->dir, "in_illuminance_scale", &ap->lux_per_count))
		ap->lux_per_count = 0;

	/* 驱动只有在设备树给了中断时才导出事件；阈值用驱动默认值（sysfs 可调） */
	snprintf(path, sizeof(path), "%s/events/in_proximity_thresh_rising_en", ap->dir);
	if (access(path, F_OK) == 0) {
		int dev_fd = open(node, O_RDONLY | O_CLOEXEC);

		if (dev_fd >= 0) {
			int fd = -1;

			if (ioctl(dev_fd, IIO_GET_EVENT_FD_IOCTL, &fd) == 0 && fd >= 0) {
				ap->event_fd = fd;
				fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
				iio_write_attr(ap->dir, "events/in_proximity_thresh_rising_en", "1");
				iio_write_attr(ap->dir, "events/in_proximity_thresh_falling_en", "1");
			}
			close(dev_fd);	/* 事件 fd 是独立的匿名 fd */
		}
	}
	return 0;
}

void ap_close(struct ap *ap)
{
	int *fds[4] = { &ap->ir_fd, &ap->als_fd, &ap->ps_fd, &ap->event_fd };
	int i;

	for (i = 0; i < 4; i++) {
		if (*fds[i] >= 0) {
			close(*fds[i]);
			*fds[i] = -1;
		}
	}
	/* 没人用就关掉接近中断 */
	if (ap->dir[0]) {
		iio_write_attr(ap->dir, "events/in_proximity_thresh_rising_en", "0");
		iio_write_attr(ap->dir, "events/in_proximity_thresh_falling_en", "0");
	}
}

/* 0 成功；-EAGAIN 该通道无效；其他负值为错误 */
static int read_value(int fd, int *value)
{
	char buf[32];
	ssize_t n;

	do {
		n = pread(fd, buf, sizeof(buf) - 1, 0);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return -errno;
	buf[n] = '\0';
	*value = atoi(buf);
	return 0;
}

int ap_read(struct ap *ap, int *ir, int *als, int *ps, int *flags, char *err)
{
	const struct {
		int fd;
		int *value;
		int valid;
		const char *name;
	} ch[3] = {
		{ ap->ir_fd, ir, AP3216C_IR_VALID, "IR" },
		{ ap->als_fd, als, AP3216C_ALS_VALID, "ALS" },
		{ ap->ps_fd, ps, AP3216C_PS_VALID, "PS" },
	};
	int i;

	*flags = 0;
	for (i = 0; i < 3; i++) {
		int ret;

		*ch[i].value = 0;
		ret = read_value(ch[i].fd, ch[i].value);
		if (ret == 0) {
			*flags |= ch[i].valid;
		} else if (ret != -EAGAIN) {
			snprintf(err, ERRLEN, "read AP3216C %s: %s", ch[i].name, strerror(-ret));
			return -1;
		}
	}
	return 0;
}

int ap_read_events(struct ap *ap, ap_event_cb cb, void *ctx, char *err)
{
	struct iio_event_data ev[8];
	int count = 0;

	for (;;) {
		ssize_t n = read(ap->event_fd, ev, sizeof(ev));
		ssize_t i;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			snprintf(err, ERRLEN, "read AP3216C events: %s", strerror(errno));
			return -1;
		}

		/* 事件码：bit63~56 类型，bit54~48 方向，bit39~32 通道类型 */
		for (i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
			unsigned type = (ev[i].id >> 56) & 0xff;
			unsigned dir = (ev[i].id >> 48) & 0x7f;
			unsigned chan = (ev[i].id >> 32) & 0xff;

			if (chan != IIO_PROXIMITY || type != IIO_EV_TYPE_THRESH
			    || (dir != IIO_EV_DIR_RISING && dir != IIO_EV_DIR_FALLING))
				continue;
			cb(dir == IIO_EV_DIR_RISING, ev[i].timestamp, ctx);
			count++;
		}
		if (n < (ssize_t)sizeof(ev))
			break;
	}
	return count;
}
