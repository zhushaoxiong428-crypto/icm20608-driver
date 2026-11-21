#ifndef SENSORWORKER_H
#define SENSORWORKER_H

#include <QObject>
#include <QString>
#include <QVector>

#include "icm20608iio.h"
#include "ap3216ciio.h"

class QTimer;
class QSocketNotifier;

class SensorWorker : public QObject
{
	Q_OBJECT

public:
	/* 每秒从 IIO 缓冲区读取几次（读取间隔 100 ms） */
	static const int kImuDrainsPerSecond = 10;

	explicit SensorWorker(QObject *parent = nullptr);
	~SensorWorker() override;

	/* IMU 采样率（Hz），须在线程启动前设置 */
	void setImuRate(int rateHz);

public slots:
	void setSampleInterval(int intervalMs);
	void start(); /* 在线程中打开设备、启动定时器 */
	void readApOnce(); /* 读取一次AP3216C数据 */

signals:
	/* AP3216C 状态与数据 */
	void ready(bool available); /*通知界面设备已准备好 */
	void apDataReady(int ir, int als, int ps, int flags,
		double luxPerCount); /*把采集结果发送给界面，照度 = als * luxPerCount */
	/* AP3216C 接近事件：来自 AP_INT 中断，near 为 true 表示物体靠近 */
	void proximityChanged(bool near, qint64 timestampNs);
	void apErrorOccurred(const QString &message); /*把错误信息发送给界面 */

	/*
	 * ICM20608：每个界面刷新周期发送一次，samples 为这段时间内 IIO 缓冲区里的全部样本，
	 * rateHz 为按内核时间戳算出的实际采样率
	 */
	void icmSamplesReady(const QVector<ImuSample> &samples, double rateHz);
	void icmErrorOccurred(const QString &message); /*把错误信息发送给界面 */

private slots:
	void readImu(); /* 定时读走 IIO 缓冲区里攒下的样本 */
	void readApEvents(); /* AP3216C 事件 fd 可读 */
	void publishImu(); /* 界面刷新周期到，把攒下的样本交给界面 */

private:
	Ap3216cIio ap;
	QSocketNotifier *apEventNotifier;
	QTimer *apTimer;

	Icm20608Iio imu;
	QTimer *imuDrainTimer; /* 每 100 ms 读一次缓冲区 */
	QTimer *imuTimer; /* 按界面采集间隔发布 */
	QVector<ImuSample> imuPending;
	int imuRateHz;
};

#endif /* SENSORWORKER_H */
