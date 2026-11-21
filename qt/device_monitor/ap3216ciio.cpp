#include "ap3216ciio.h"
#include "ap3216c_data.h"

#include <QDir>
#include <QFile>
#include <QStringList>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>

/*
 * 工具链的内核头文件没有 linux/iio/，按 include/uapi/linux/iio/events.h、types.h 定义：
 * 事件码 id：bit63~56 事件类型，bit54~48 方向，bit39~32 通道类型
 */
struct IioEventData {
	quint64 id;
	qint64 timestamp;
};
#define IIO_GET_EVENT_FD_IOCTL _IOR('i', 0x90, int)
static const unsigned kIioProximity = 8; /* enum iio_chan_type */
static const unsigned kIioEvThresh = 0; /* enum iio_event_type */
static const unsigned kIioEvDirRising = 1; /* enum iio_event_direction */
static const unsigned kIioEvDirFalling = 2;

static const char *const kIioRoot = "/sys/bus/iio/devices";

Ap3216cIio::Ap3216cIio()
	: m_irFd(-1)
	, m_alsFd(-1)
	, m_psFd(-1)
	, m_eventFd(-1)
	, m_alsScale(0)
{
}

Ap3216cIio::~Ap3216cIio()
{
	close();
}

bool Ap3216cIio::findDevice()
{
	QDir root(kIioRoot);
	const QStringList devices = root.entryList(QStringList() << "iio:device*",
		QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);

	for (const QString &device : devices) {
		QFile file(root.filePath(device + "/name"));
		if (file.open(QIODevice::ReadOnly) && file.readAll().trimmed() == "ap3216c") {
			m_dir = root.filePath(device);
			m_devNode = "/dev/" + device;
			return true;
		}
	}

	return false;
}

bool Ap3216cIio::writeAttr(const QString &attr, const QByteArray &value)
{
	QFile file(m_dir + "/" + attr);
	return file.open(QIODevice::WriteOnly | QIODevice::Unbuffered)
		&& file.write(value) == value.size();
}

bool Ap3216cIio::open(QString *error)
{
	close();

	if (!findDevice()) {
		*error = "AP3216C IIO device not found (is ap3216c.ko loaded?)";
		return false;
	}

	const struct {
		const char *attr;
		int *fd;
	} channels[] = {
		{ "in_intensity_ir_raw", &m_irFd },
		{ "in_illuminance_raw", &m_alsFd },
		{ "in_proximity_raw", &m_psFd },
	};
	for (const auto &channel : channels) {
		const QString path = m_dir + "/" + channel.attr;
		*channel.fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
		if (*channel.fd < 0) {
			*error = QString("open %1 failed: %2")
				.arg(path, QString::fromLocal8Bit(strerror(errno)));
			close();
			return false;
		}
	}

	QFile scale(m_dir + "/in_illuminance_scale");
	bool ok = false;
	if (scale.open(QIODevice::ReadOnly)) {
		m_alsScale = scale.readAll().trimmed().toDouble(&ok);
	}
	if (!ok) {
		m_alsScale = 0;
	}

	/* 驱动只有在设备树给了中断时才导出事件；阈值用驱动默认值（可在 sysfs 调整） */
	if (QFile::exists(m_dir + "/events/in_proximity_thresh_rising_en")) {
		const int devFd = ::open(m_devNode.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
		if (devFd >= 0) {
			int eventFd = -1;
			if (::ioctl(devFd, IIO_GET_EVENT_FD_IOCTL, &eventFd) == 0 && eventFd >= 0) {
				m_eventFd = eventFd;
				::fcntl(m_eventFd, F_SETFL, ::fcntl(m_eventFd, F_GETFL) | O_NONBLOCK);
				writeAttr("events/in_proximity_thresh_rising_en", "1");
				writeAttr("events/in_proximity_thresh_falling_en", "1");
			}
			::close(devFd); /* 事件 fd 是独立的匿名 fd，关掉设备节点不影响它 */
		}
	}

	return true;
}

void Ap3216cIio::close()
{
	for (int *fd : { &m_irFd, &m_alsFd, &m_psFd, &m_eventFd }) {
		if (*fd >= 0) {
			::close(*fd);
			*fd = -1;
		}
	}

	/* 程序退出后不再需要接近中断 */
	if (!m_dir.isEmpty() && QFile::exists(m_dir + "/events/in_proximity_thresh_rising_en")) {
		writeAttr("events/in_proximity_thresh_rising_en", "0");
		writeAttr("events/in_proximity_thresh_falling_en", "0");
	}
}

int Ap3216cIio::readValue(int fd, int *value) const
{
	char buf[32];
	ssize_t n;

	do {
		n = ::pread(fd, buf, sizeof(buf) - 1, 0);
	} while (n < 0 && errno == EINTR);

	if (n < 0) {
		return -errno;
	}
	buf[n] = '\0';
	*value = atoi(buf);
	return 0;
}

bool Ap3216cIio::read(int *ir, int *als, int *ps, int *flags, QString *error)
{
	const struct {
		int fd;
		int *value;
		int validFlag;
		const char *name;
	} channels[] = {
		{ m_irFd, ir, AP3216C_IR_VALID, "IR" },
		{ m_alsFd, als, AP3216C_ALS_VALID, "ALS" },
		{ m_psFd, ps, AP3216C_PS_VALID, "PS" },
	};

	*flags = 0;
	for (const auto &channel : channels) {
		*channel.value = 0;
		const int ret = readValue(channel.fd, channel.value);
		if (ret == 0) {
			*flags |= channel.validFlag;
		} else if (ret != -EAGAIN) { /* EAGAIN：IR 溢出，数据无效但不算错误 */
			*error = QString("read AP3216C %1 failed: %2")
				.arg(channel.name, QString::fromLocal8Bit(strerror(-ret)));
			return false;
		}
	}

	return true;
}

int Ap3216cIio::readEvents(QVector<ProximityEvent> &out, QString *error)
{
	IioEventData events[8];
	int count = 0;

	for (;;) {
		const ssize_t n = ::read(m_eventFd, events, sizeof(events));
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			*error = QString("read AP3216C events failed: %1")
				.arg(QString::fromLocal8Bit(strerror(errno)));
			return -1;
		}

		for (ssize_t i = 0; i < n / ssize_t(sizeof(IioEventData)); i++) {
			const quint64 id = events[i].id;
			const unsigned type = (id >> 56) & 0xff;
			const unsigned dir = (id >> 48) & 0x7f;
			const unsigned chanType = (id >> 32) & 0xff;
			if (chanType != kIioProximity || type != kIioEvThresh
				|| (dir != kIioEvDirRising && dir != kIioEvDirFalling)) {
				continue;
			}
			out.append({ dir == kIioEvDirRising, events[i].timestamp });
			count++;
		}

		if (n < ssize_t(sizeof(events))) {
			break;
		}
	}

	return count;
}
