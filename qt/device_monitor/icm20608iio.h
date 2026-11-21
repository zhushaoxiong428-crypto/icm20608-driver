#ifndef ICM20608IIO_H
#define ICM20608IIO_H

#include <QMetaType>
#include <QString>
#include <QVector>

/* 一个 IMU 样本，已按 IIO 的 scale/offset 换算成物理量 */
struct ImuSample {
	double accel[3]; /* m/s^2 */
	double gyro[3]; /* rad/s */
	double tempC; /* 芯片温度，摄氏度 */
	qint64 timestampNs; /* 内核在数据就绪中断上半部记录的时间（CLOCK_REALTIME） */
};
Q_DECLARE_METATYPE(ImuSample)
Q_DECLARE_METATYPE(QVector<ImuSample>)

/*
 * 通过 IIO 缓冲区读取 ICM-20608（驱动：third_party/icm20608-driver）。
 *
 * 内核侧：数据就绪中断 -> 上半部记时间戳 -> 线程化下半部 SPI 读 14 字节 -> 推入 kfifo。
 * 应用侧：open() 打开全部扫描通道、设置采样率和 watermark 并启用缓冲区；
 *         readSamples() 非阻塞地一次读走缓冲区里的全部记录（调用方定时调用，或 poll(fd)
 *         等到攒够 watermark 条再调用）。
 * 记录布局从 scan_elements/<通道>_index 与 _type 解析，不写死 24 字节结构。
 */
class Icm20608Iio
{
public:
	Icm20608Iio();
	~Icm20608Iio();

	/* rateHz：芯片采样率；wakeupsPerSecond：应用每秒读取几次（决定 watermark 和读缓冲大小） */
	bool open(int rateHz, int wakeupsPerSecond, QString *error);
	void close();

	int fd() const { return m_fd; }
	int rateHz() const { return m_rateHz; }
	QString deviceDir() const { return m_dir; }

	/* 非阻塞读出缓冲区中现有的全部记录，追加到 out；返回读到的样本数，出错返回 -1 */
	int readSamples(QVector<ImuSample> &out, QString *error);

private:
	struct ScanElement {
		QString name; /* 如 in_accel_x */
		int index; /* 在记录中的顺序 */
		bool bigEndian;
		bool isSigned;
		int realBits;
		int storageBytes;
		int shift;
		int offset; /* 在记录中的字节偏移 */
	};

	enum Field {
		AccelX, AccelY, AccelZ, Temp, GyroX, GyroY, GyroZ, Timestamp, FieldCount
	};

	bool findDevice();
	bool writeAttr(const QString &attr, const QByteArray &value, QString *error);
	QByteArray readAttr(const QString &attr, bool *ok = nullptr) const;
	double readDoubleAttr(const QString &attr, bool *ok) const;
	bool parseScanLayout(QString *error);
	static qint64 extract(const unsigned char *record, const ScanElement &element);

	QString m_dir; /* /sys/bus/iio/devices/iio:deviceN */
	QString m_devNode; /* /dev/iio:deviceN */
	int m_fd;
	int m_rateHz;
	int m_recordSize;
	int m_fieldElement[FieldCount]; /* 字段 -> m_elements 下标，-1 表示没有 */
	QVector<ScanElement> m_elements;
	double m_accelScale;
	double m_gyroScale;
	double m_tempScale;
	double m_tempOffset;
	QByteArray m_buffer;
};

#endif /* ICM20608IIO_H */
