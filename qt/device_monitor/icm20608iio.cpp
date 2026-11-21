#include "icm20608iio.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static const char *const kIioRoot = "/sys/bus/iio/devices";

Icm20608Iio::Icm20608Iio()
	: m_fd(-1)
	, m_rateHz(0)
	, m_recordSize(0)
	, m_accelScale(0)
	, m_gyroScale(0)
	, m_tempScale(0)
	, m_tempOffset(0)
{
	std::fill(m_fieldElement, m_fieldElement + FieldCount, -1);
}

Icm20608Iio::~Icm20608Iio()
{
	close();
}

/* 驱动按芯片型号注册 IIO 设备名：ICM-20608-D 为 "icm20608d"，-G 为 "icm20608" */
bool Icm20608Iio::findDevice()
{
	QDir root(kIioRoot);
	const QStringList devices = root.entryList(QStringList() << "iio:device*",
		QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);

	for (const QString &device : devices) {
		QFile file(root.filePath(device + "/name"));
		if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		const QByteArray name = file.readAll().trimmed();
		if (name == "icm20608d" || name == "icm20608") {
			m_dir = root.filePath(device);
			m_devNode = "/dev/" + device;
			return true;
		}
	}

	return false;
}

bool Icm20608Iio::writeAttr(const QString &attr, const QByteArray &value, QString *error)
{
	/* sysfs 写入要不经缓冲直接下发，驱动返回的 EINVAL/EBUSY 才能在 write 时拿到 */
	QFile file(m_dir + "/" + attr);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Unbuffered)
		|| file.write(value) != value.size()) {
		if (error) {
			*error = QString("write %1 = %2 failed: %3")
				.arg(attr, QString::fromLatin1(value), file.errorString());
		}
		return false;
	}

	return true;
}

QByteArray Icm20608Iio::readAttr(const QString &attr, bool *ok) const
{
	QFile file(m_dir + "/" + attr);
	const bool opened = file.open(QIODevice::ReadOnly);
	if (ok) {
		*ok = opened;
	}

	return opened ? file.readAll().trimmed() : QByteArray();
}

double Icm20608Iio::readDoubleAttr(const QString &attr, bool *ok) const
{
	bool readOk = false;
	bool numberOk = false;
	const double value = readAttr(attr, &readOk).toDouble(&numberOk);
	*ok = readOk && numberOk;

	return value;
}

/*
 * 根据 scan_elements 计算一条记录里每个通道的位置：
 * 按 _index 排序，每个通道按自身存储字节数对齐，整条记录按最大的存储字节数对齐。
 * 本驱动为 7 x be:s16/16 + 2 字节填充 + le:s64/64 时间戳 = 24 字节。
 */
bool Icm20608Iio::parseScanLayout(QString *error)
{
	static const QRegularExpression typeRe("^(be|le):(s|u)(\\d+)/(\\d+)>>(\\d+)$");
	static const struct {
		const char *name;
		Field field;
	} fieldNames[] = {
		{ "in_accel_x", AccelX }, { "in_accel_y", AccelY }, { "in_accel_z", AccelZ },
		{ "in_temp", Temp },
		{ "in_anglvel_x", GyroX }, { "in_anglvel_y", GyroY }, { "in_anglvel_z", GyroZ },
		{ "in_timestamp", Timestamp },
	};

	QDir scanDir(m_dir + "/scan_elements");
	const QStringList enables = scanDir.entryList(QStringList() << "*_en", QDir::Files);

	m_elements.clear();
	for (const QString &enable : enables) {
		if (readAttr("scan_elements/" + enable) != "1") {
			continue;
		}

		ScanElement element;
		element.name = enable.left(enable.size() - 3);

		bool ok = false;
		element.index = readAttr("scan_elements/" + element.name + "_index").toInt(&ok);
		const QString type = QString::fromLatin1(
			readAttr("scan_elements/" + element.name + "_type"));
		const QRegularExpressionMatch match = typeRe.match(type);
		if (!ok || !match.hasMatch() || match.captured(4).toInt() % 8 != 0) {
			*error = QString("unsupported scan element %1 (type \"%2\")")
				.arg(element.name, type);
			return false;
		}

		element.bigEndian = match.captured(1) == "be";
		element.isSigned = match.captured(2) == "s";
		element.realBits = match.captured(3).toInt();
		element.storageBytes = match.captured(4).toInt() / 8;
		element.shift = match.captured(5).toInt();
		element.offset = 0;
		m_elements.append(element);
	}

	std::sort(m_elements.begin(), m_elements.end(),
		[](const ScanElement &a, const ScanElement &b) { return a.index < b.index; });

	int offset = 0;
	int maxBytes = 1;
	for (ScanElement &element : m_elements) {
		const int bytes = element.storageBytes;
		if (offset % bytes) {
			offset += bytes - offset % bytes;
		}
		element.offset = offset;
		offset += bytes;
		maxBytes = std::max(maxBytes, bytes);
	}
	m_recordSize = (offset % maxBytes) ? (offset + maxBytes - offset % maxBytes) : offset;

	std::fill(m_fieldElement, m_fieldElement + FieldCount, -1);
	for (int i = 0; i < m_elements.size(); i++) {
		for (const auto &fieldName : fieldNames) {
			if (m_elements[i].name == QLatin1String(fieldName.name)) {
				m_fieldElement[fieldName.field] = i;
			}
		}
	}

	for (int field = AccelX; field <= GyroZ; field++) {
		if (m_fieldElement[field] < 0) {
			*error = "IIO scan layout lacks accel/gyro/temp channels";
			return false;
		}
	}

	return m_recordSize > 0;
}

/* 按 _type 描述取出一个通道的原始值：字节序 -> 右移 -> 截取有效位 -> 符号扩展 */
qint64 Icm20608Iio::extract(const unsigned char *record, const ScanElement &element)
{
	quint64 raw = 0;
	for (int i = 0; i < element.storageBytes; i++) {
		const int pos = element.bigEndian ? i : element.storageBytes - 1 - i;
		raw = (raw << 8) | record[element.offset + pos];
	}

	raw >>= element.shift;
	if (element.realBits < 64) {
		const quint64 mask = (quint64(1) << element.realBits) - 1;
		raw &= mask;
		if (element.isSigned && (raw & (quint64(1) << (element.realBits - 1)))) {
			raw |= ~mask;
		}
	}

	return qint64(raw);
}

bool Icm20608Iio::open(int rateHz, int wakeupsPerSecond, QString *error)
{
	close();

	if (!findDevice()) {
		*error = "ICM20608 IIO device not found (is icm20608.ko loaded?)";
		return false;
	}

	/* 缓冲区开着时不能改通道和采样率；上次异常退出可能没关，先关掉 */
	if (!writeAttr("buffer/enable", "0", error)) {
		return false;
	}

	/* IIO 约定：物理量 = (raw + offset) * scale，加速度 m/s^2、角速度 rad/s、温度毫摄氏度 */
	bool okA = false;
	bool okG = false;
	bool okT = false;
	bool okO = false;
	m_accelScale = readDoubleAttr("in_accel_scale", &okA);
	m_gyroScale = readDoubleAttr("in_anglvel_scale", &okG);
	m_tempScale = readDoubleAttr("in_temp_scale", &okT);
	m_tempOffset = readDoubleAttr("in_temp_offset", &okO);
	if (!okA || !okG || !okT || !okO) {
		*error = "read IIO scale/offset failed";
		return false;
	}

	/* 驱动的 available_scan_masks 只允许全部通道一起打开 */
	QDir scanDir(m_dir + "/scan_elements");
	for (const QString &enable : scanDir.entryList(QStringList() << "*_en", QDir::Files)) {
		if (!writeAttr("scan_elements/" + enable, "1", error)) {
			return false;
		}
	}

	if (!writeAttr("sampling_frequency", QByteArray::number(rateHz), error)) {
		return false;
	}
	bool okRate = false;
	m_rateHz = readAttr("sampling_frequency").toInt(&okRate); /* 芯片按分频取整，以回读为准 */
	if (!okRate || m_rateHz <= 0) {
		m_rateHz = rateHz;
	}

	if (!parseScanLayout(error)) {
		return false;
	}

	/*
	 * watermark：缓冲区攒够这么多条才让 poll 返回可读，应用一次读走一批，
	 * 唤醒次数从每样本一次降到每秒 wakeupsPerSecond 次。length 留出几秒余量
	 */
	const int watermark = std::max(1, m_rateHz / std::max(1, wakeupsPerSecond));
	const int length = std::max(watermark * 4, m_rateHz * 4);
	if (!writeAttr("buffer/length", QByteArray::number(length), error)) {
		return false;
	}
	/* 较老的内核没有 buffer/watermark，此时退化为每个样本唤醒一次，不算错误 */
	writeAttr("buffer/watermark", QByteArray::number(watermark), nullptr);
	if (!writeAttr("buffer/enable", "1", error)) {
		return false;
	}

	m_fd = ::open(m_devNode.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (m_fd < 0) {
		*error = QString("open %1 failed: %2")
			.arg(m_devNode, QString::fromLocal8Bit(strerror(errno)));
		writeAttr("buffer/enable", "0", nullptr);
		return false;
	}

	m_buffer.resize(m_recordSize * std::max(64, watermark * 2));
	return true;
}

void Icm20608Iio::close()
{
	if (m_fd >= 0) {
		::close(m_fd);
		m_fd = -1;
	}

	/* 关掉缓冲区，芯片停止产生数据就绪中断；下次启动才能重新设置采样率 */
	if (!m_dir.isEmpty()) {
		writeAttr("buffer/enable", "0", nullptr);
	}
}

int Icm20608Iio::readSamples(QVector<ImuSample> &out, QString *error)
{
	if (m_fd < 0) {
		*error = "ICM20608 IIO buffer not open";
		return -1;
	}

	int count = 0;
	for (;;) {
		const ssize_t n = ::read(m_fd, m_buffer.data(), m_buffer.size());
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			*error = QString("read %1 failed: %2")
				.arg(m_devNode, QString::fromLocal8Bit(strerror(errno)));
			return -1;
		}
		if (n == 0) {
			break;
		}

		const unsigned char *data = reinterpret_cast<const unsigned char *>(m_buffer.constData());
		for (ssize_t pos = 0; pos + m_recordSize <= n; pos += m_recordSize) {
			const unsigned char *record = data + pos;
			ImuSample sample;
			for (int axis = 0; axis < 3; axis++) {
				sample.accel[axis] = extract(record, m_elements[m_fieldElement[AccelX + axis]])
					* m_accelScale;
				sample.gyro[axis] = extract(record, m_elements[m_fieldElement[GyroX + axis]])
					* m_gyroScale;
			}
			sample.tempC = (extract(record, m_elements[m_fieldElement[Temp]]) + m_tempOffset)
				* m_tempScale / 1000.0;
			sample.timestampNs = m_fieldElement[Timestamp] >= 0
				? extract(record, m_elements[m_fieldElement[Timestamp]]) : 0;
			out.append(sample);
			count++;
		}

		/* 没读满说明缓冲区已经读空 */
		if (n < m_buffer.size()) {
			break;
		}
	}

	return count;
}
