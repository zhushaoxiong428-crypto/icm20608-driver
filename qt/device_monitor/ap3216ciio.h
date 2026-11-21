#ifndef AP3216CIIO_H
#define AP3216CIIO_H

#include <QString>
#include <QVector>

/*
 * 通过 IIO 读取 AP3216C（驱动：drivers/ap3216c.c）。
 *
 * 数值：in_intensity_ir_raw / in_illuminance_raw / in_proximity_raw，驱动每个通道一次 I2C 传输。
 *       IR 溢出时 IR、PS 文件读返回 EAGAIN，对应通道标为无效。sysfs fd 打开一次，每次 pread 偏移 0。
 * 事件：AP_INT 中断 -> 驱动推送 IIO 事件（PS 越过阈值：rising = 远到近，falling = 近到远）。
 *       对 /dev/iio:deviceN 做 IIO_GET_EVENT_FD_IOCTL 得到事件 fd，可读时 readEvents()。
 */
class Ap3216cIio
{
public:
	struct ProximityEvent {
		bool near;
		qint64 timestampNs; /* 驱动在中断上半部记录，CLOCK_REALTIME */
	};

	Ap3216cIio();
	~Ap3216cIio();

	bool open(QString *error);
	void close();

	/* flags 使用 ap3216c_data.h 中的 AP3216C_*_VALID；返回 false 表示读失败（不含单通道无效） */
	bool read(int *ir, int *als, int *ps, int *flags, QString *error);
	double luxPerCount() const { return m_alsScale; }

	/* 驱动没有中断时（设备树未配置）为 -1 */
	int eventFd() const { return m_eventFd; }
	int readEvents(QVector<ProximityEvent> &out, QString *error);

private:
	bool findDevice();
	int readValue(int fd, int *value) const; /* 0 成功，-EAGAIN 无效，其他负值为错误 */
	bool writeAttr(const QString &attr, const QByteArray &value);

	QString m_dir;
	QString m_devNode;
	int m_irFd;
	int m_alsFd;
	int m_psFd;
	int m_eventFd;
	double m_alsScale;
};

#endif /* AP3216CIIO_H */
