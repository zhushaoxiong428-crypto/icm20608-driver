#include "sensorworker.h"
#include "ap3216c_data.h"

#include <QTimer>
#include <QDebug>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

SensorWorker::SensorWorker(QObject *parent)
	: QObject(parent)
	, apFd(-1)
	, apTimer(new QTimer(this))
	, imuDrainTimer(new QTimer(this))
	, imuTimer(new QTimer(this))
	, imuRateHz(100)
{
	apTimer->setInterval(1000);
	imuTimer->setInterval(1000);

	connect(apTimer, &QTimer::timeout,
		this, &SensorWorker::readApOnce);

	connect(imuTimer, &QTimer::timeout,
		this, &SensorWorker::publishImu);

	imuDrainTimer->setInterval(1000 / kImuDrainsPerSecond);
	connect(imuDrainTimer, &QTimer::timeout,
		this, &SensorWorker::readImu);
}

SensorWorker::~SensorWorker()
{
	apTimer->stop();
	imuDrainTimer->stop();
	imuTimer->stop();

	if (apFd >= 0) {
		if (::close(apFd) < 0) {
			qWarning() << "close AP3216C failed:" << strerror(errno);
		}
	}

	/* imu 析构时关闭缓冲区，芯片停止产生数据就绪中断 */
}

void SensorWorker::setImuRate(int rateHz)
{
	imuRateHz = rateHz;
}

void SensorWorker::setSampleInterval(int intervalMs)
{
	/* 活跃的定时器会按新间隔重新计时，未启动的只更新间隔。 */
	apTimer->setInterval(intervalMs);
	imuTimer->setInterval(intervalMs);
}

void SensorWorker::start()
{
	apFd = ::open("/dev/ap3216c", O_RDONLY);
	if (apFd < 0) {
		emit apErrorOccurred(
			QString("open AP3216C failed: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
	} else {
		apTimer->start(); /* 使用设置的采集间隔 */
		emit ready(true); /* 用来通知这个界面手动按钮启用 */
		readApOnce();
	}

	/*
	 * IMU 走 IIO 缓冲区：采集由数据就绪中断驱动，样本和时间戳在内核里进 kfifo，不会丢。
	 * 应用每 100 ms 把攒下的样本一次读走，而不是 poll 这个 fd：
	 * 4.1 内核每推入一个样本都会唤醒 poll 等待者（industrialio-buffer.c 的
	 * iio_push_to_buffer，watermark 只决定 poll 是否返回），实测 100 Hz 时
	 * QSocketNotifier 方式线程每秒被唤醒约 126 次，定时读约 12 次
	 */
	QString error;
	if (!imu.open(imuRateHz, kImuDrainsPerSecond, &error)) {
		emit icmErrorOccurred(error);
		return;
	}

	qInfo().noquote() << "ICM20608 IIO:" << imu.deviceDir() << imu.rateHz() << "Hz";
	imuDrainTimer->start();
	imuTimer->start();
}

void SensorWorker::readApOnce()
{
	struct ap3216c_data data;
	ssize_t ret;

	do {
		ret = ::read(apFd, &data, sizeof(data));
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		emit apErrorOccurred(
			QString("read AP3216C failed: %1")
				.arg(QString::fromLocal8Bit(strerror(errno))));
		return;
	}

	emit apDataReady(data.ir, data.als, data.ps, data.flags);
}

void SensorWorker::readImu()
{
	QString error;
	if (imu.readSamples(imuPending, &error) < 0) {
		imuDrainTimer->stop();
		imuTimer->stop();
		emit icmErrorOccurred(error);
	}
}

void SensorWorker::publishImu()
{
	readImu(); /* 先把缓冲区里剩下的读完，避免最后不到 100 ms 的样本挪到下一周期 */

	if (imuPending.isEmpty()) {
		/* 有采样率却一个样本都没有，多半是数据就绪中断没来 */
		emit icmErrorOccurred("no ICM20608 samples in the last interval");
		return;
	}

	/* 用内核在中断里记的时间戳算实际采样率，与界面线程的调度无关 */
	double rateHz = 0;
	const qint64 spanNs = imuPending.last().timestampNs - imuPending.first().timestampNs;
	if (imuPending.size() > 1 && spanNs > 0) {
		rateHz = (imuPending.size() - 1) * 1e9 / spanNs;
	}

	emit icmSamplesReady(imuPending, rateHz);
	imuPending.clear();
}
