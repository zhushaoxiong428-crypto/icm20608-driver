#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ap3216c_data.h"
#include "sensorworker.h"

#include <QDebug>
#include <QMessageBox>
#include <QCoreApplication>
#include <QSettings>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegExp>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/input.h>

/*
 * 查找按键的 input 节点。eventX 编号随驱动加载顺序变化（如触摸屏先加载就会占用较小编号），
 * 不能写死，这里按 key_drv 注册的设备名 "keyinput" 在 sysfs 中查找。
 * 也可用环境变量 DEVICE_MONITOR_KEY_DEV 直接指定，如 /dev/input/event1。
 */
static QString findKeyDevice()
{
	const QByteArray env = qgetenv("DEVICE_MONITOR_KEY_DEV");
	if (!env.isEmpty()) {
		return QString::fromLocal8Bit(env);
	}

	QDir dir("/sys/class/input");
	const QStringList events = dir.entryList(QStringList() << "event*",
		QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
	for (const QString &event : events) {
		QFile name(dir.filePath(event + "/device/name"));
		if (name.open(QIODevice::ReadOnly) && name.readAll().trimmed() == "keyinput") {
			return "/dev/input/" + event;
		}
	}

	return QString();
}

/* 冒号后边叫成员初始化列表，进入函数体 {} 之前执行 */
MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent) /* 初始化基类窗口 */
	, ui(new Ui::MainWindow) /* Ui::MainWindow *ui = new Ui::MainWindow; */
	, ledFd(-1)
	, ledOn(false)
	, autoMode(false)
	, lightOnThreshold(20)
	, lightOffThreshold(50)
	, sampleIntervalMs(1000)
	, imuRateHz(100)
	, keyFd(-1)
	, keyNotifier(nullptr)
	, Worker(new SensorWorker)
	, proximityState(-1)
	, proximityEvents(0)
	, recording(false)
	, recordStartNs(0)
{
	ui->setupUi(this); /* 在当前窗口上创建、配置控件，this == &w */
	ui->autoModeCheckBox->setEnabled(false);

	ui->apStatusLabel->setText("AP3216C: WAITING");
	ui->icmStatusLabel->setText("ICM20608: WAITING");

	/* 设备打开成功后，再启用手动读取按钮 */
	ui->readSensorButton->setEnabled(false);

	loadSettings();

	/* 此时采集对象及其定时器仍属于界面线程，先设置初始间隔。 */
	Worker->setSampleInterval(sampleIntervalMs);
	Worker->setImuRate(imuRateHz);

	/* 跨线程（队列连接）传递样本批次需要注册类型 */
	qRegisterMetaType<QVector<ImuSample>>("QVector<ImuSample>");
	Worker->moveToThread(&Thread);

	/* 工作线程启动后，安排设备初始化 */
	connect(&Thread, &QThread::started,
		Worker, &SensorWorker::start,
		Qt::QueuedConnection);

	/* 工作线程结束后，在所属线程中销毁采集对象 */
	connect(&Thread, &QThread::finished,
		Worker, &QObject::deleteLater);

	/* 界面发出手动采集请求 */
	connect(this, &MainWindow::requestApRead,
		Worker, &SensorWorker::readApOnce);

	/* 运行期间，定时器间隔必须交给所属工作线程修改。 */
	connect(this, &MainWindow::requestSampleInterval,
		Worker, &SensorWorker::setSampleInterval,
		Qt::QueuedConnection);

	/* 设备就绪后启用按钮 */
	connect(Worker, &SensorWorker::ready,
		ui->readSensorButton, &QPushButton::setEnabled);

	/* 采集结果、错误信息交给界面线程处理 */
	connect(Worker, &SensorWorker::apDataReady,
		this, &MainWindow::updateApData);

	connect(Worker, &SensorWorker::apErrorOccurred,
		this, &MainWindow::showApError);

	connect(Worker, &SensorWorker::proximityChanged,
		this, &MainWindow::updateProximity);

	connect(Worker, &SensorWorker::icmSamplesReady,
		this, &MainWindow::updateIcmData);

	connect(Worker, &SensorWorker::icmErrorOccurred,
		this, &MainWindow::showIcmError);

	Thread.start(); /* 启动工作线程 */

	const QString keyDevice = findKeyDevice();
	if (keyDevice.isEmpty()) {
		qWarning() << "key input device \"keyinput\" not found";
	} else {
		keyFd = ::open(keyDevice.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
		if (keyFd < 0) {
			qWarning() << "open key" << keyDevice << "failed:" << strerror(errno);
		}
	}

	if (keyFd < 0) {
		ui->keyLabel->setText("KEY: unavailable");
	} else {
		keyNotifier = new QSocketNotifier(
			keyFd, QSocketNotifier::Read, this); /* 启用监听可读事件 */

		connect(keyNotifier, &QSocketNotifier::activated,
			this, &MainWindow::readKeyData);

		ui->keyLabel->setText("KEY: READY");
	}

	ledFd = ::open("/dev/led0", O_WRONLY);
	if (ledFd < 0) {
		qWarning() << "open /dev/led0 failed:" << strerror(errno);

		ui->pushButton->setText("LED unavailable");
		ui->pushButton->setEnabled(false); /* LED 按钮被禁用 */
		return;
	}

	if (!setLed(false)) {
		::close(ledFd);
		ledFd = -1;

		ui->pushButton->setText("LED init failed");
		ui->pushButton->setEnabled(false);
	} else {
		ui->autoModeCheckBox->setEnabled(true); /* led初始化成功启用自动模式开关灯 */
	}
}

MainWindow::~MainWindow()
{
	qDebug() << "MainWindow destructor called"; /* 打印到串口终端 */

	Thread.quit();
	Thread.wait();
	stopRecording();

	if (keyNotifier != nullptr) {
		keyNotifier->setEnabled(false);
	}

	if (keyFd >= 0) {
		if (::close(keyFd) < 0) {
			qWarning() << "close key failed:" << strerror(errno);
		}
	}

	if (ledFd >= 0) {
		setLed(false);

		if (::close(ledFd) < 0) {
			qWarning() << "close LED failed:" << strerror(errno);
		}

		ledFd = -1;
	}

	delete ui;
}

bool MainWindow::setLed(bool on)
{
	char value = on ? '1' : '0';
	ssize_t ret;

	do {
		ret = ::write(ledFd, &value, sizeof(value));
	} while (ret < 0 && errno == EINTR);

	if (ret != static_cast<ssize_t>(sizeof(value))) {
		if (ret < 0) {
			qWarning() << "write LED failed:" << strerror(errno);
		} else {
			qWarning() << "write LED incomplete:" << ret;
		}

		return false;
	}

	ledOn = on;

	ui->pushButton->setText(ledOn ? "LED ON - Turn OFF" : "LED OFF - Turn ON");

	return true;
}

void MainWindow::controlLedByLight(int als)
{
	bool targetOn = ledOn;

	if (als <= lightOnThreshold) {
		targetOn = true;
	} else if (als >= lightOffThreshold) {
		targetOn = false;
	}

	if (targetOn == ledOn) {
		return;
	}

	if (!setLed(targetOn)) {
		ui->statusbar->showMessage(
			"Auto LED control failed", 5000);
	}
}

void MainWindow::on_pushButton_clicked()
{
	if (!setLed(!ledOn)) {
		QMessageBox::warning(
			this, "LED error", "LED write failed. Check the terminal output.");
	}
}

void MainWindow::on_exitButton_clicked()
{
	close(); /* 关闭屏幕 */
}

void MainWindow::on_readSensorButton_clicked()
{
	emit requestApRead();
}

void MainWindow::readKeyData()
{
	struct input_event event;
	ssize_t ret;

	while (true) {
		ret = ::read(keyFd, &event, sizeof(event));

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return;
			}

			qWarning() << "read key failed:" << strerror(errno);
			keyNotifier->setEnabled(false); /* 禁用按键文件描述符的监听 */
			ui->keyLabel->setText("KEY: read error");
			return;
		}

		if (ret == 0) {
			keyNotifier->setEnabled(false);
			ui->keyLabel->setText("KEY: disconnected");
			return;
		}

		if (event.type != EV_KEY || event.code != KEY_ENTER) {
			continue;
		}

		switch (event.value) {
		case 0:
			ui->keyLabel->setText("KEY: RELEASED");
			break;

		case 1:
			ui->keyLabel->setText("KEY: PRESSED");

			if (ui->pushButton->isEnabled()) {
				if (!setLed(!ledOn)) {
					qWarning() << "key: LED control failed";
				}
			}
			break;

		default:
			break;
		}
	}
}

void MainWindow::on_autoModeCheckBox_toggled(bool checked)
{
	autoMode = checked;

	ui->pushButton->setEnabled(!autoMode);

	if (autoMode) {
		ui->statusbar->showMessage(
			"Auto mode: waiting for valid ALS data", 5000);
	} else {
		ui->statusbar->showMessage("Manual mode", 5000);
	}
}

void MainWindow::updateApData(int ir, int als, int ps, int flags, double luxPerCount)
{
	/* 接近状态来自中断事件（实时），PS 数值按采集间隔刷新 */
	static const char *const kProximityText[] = { "", "  FAR", "  NEAR" };
	const char *proximity = kProximityText[proximityState + 1];

	ui->apStatusLabel->setText("AP3216C: OK");
	ui->apStatusLabel->setStyleSheet("color: #89d185;");
	ui->apStatusLabel->setToolTip(QString());

	ui->irLabel->setText((flags & AP3216C_IR_VALID)
		? QString("IR: %1").arg(ir)
		: QString("IR: invalid"));

	ui->alsLabel->setText((flags & AP3216C_ALS_VALID)
		? QString("ALS: %1 (%2 lx)").arg(als).arg(als * luxPerCount, 0, 'f', 1)
		: QString("ALS: invalid"));

	ui->psLabel->setText((flags & AP3216C_PS_VALID)
		? QString("PS: %1%2").arg(ps).arg(proximity)
		: QString("PS: invalid%1").arg(proximity));

	if (recording) {
		/* 无效通道留空，flags 保留本次各通道的有效标志。 */
		QString row = QString("%1,%2,%3,%4,%5,%6\n")
			.arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
			.arg((flags & AP3216C_IR_VALID) ? QString::number(ir) : QString())
			.arg((flags & AP3216C_ALS_VALID) ? QString::number(als) : QString())
			.arg((flags & AP3216C_ALS_VALID) ? QString::number(als * luxPerCount, 'f', 2) : QString())
			.arg((flags & AP3216C_PS_VALID) ? QString::number(ps) : QString())
			.arg(flags);

		appendRecord(apRecordFile, row.toUtf8());
	}

	if (autoMode && (flags & AP3216C_ALS_VALID)) {
		controlLedByLight(als);
	}
}

void MainWindow::updateProximity(bool near, qint64 timestampNs)
{
	proximityState = near ? 1 : 0;
	proximityEvents++;

	/* 不等下一次采集，立即更新 PS 一栏的状态后缀 */
	QString text = ui->psLabel->text();
	text.remove(QRegExp("  (NEAR|FAR)$"));
	ui->psLabel->setText(text + (near ? "  NEAR" : "  FAR"));

	const QDateTime time = QDateTime::fromMSecsSinceEpoch(timestampNs / 1000000);
	ui->statusbar->showMessage(QString("Proximity %1 (interrupt #%2 at %3)")
		.arg(near ? "NEAR" : "FAR")
		.arg(proximityEvents)
		.arg(time.toString("HH:mm:ss.zzz")), 5000);
}

void MainWindow::showApError(const QString &message)
{
	qWarning() << message;

	ui->apStatusLabel->setText("AP3216C: ERROR");
	ui->apStatusLabel->setStyleSheet("color: #f48771;");
	ui->apStatusLabel->setToolTip(message);

	ui->irLabel->setText("IR: --");
	ui->alsLabel->setText("ALS: --");
	ui->psLabel->setText("PS: --");

	ui->statusbar->showMessage(message, 5000);
}

/* IIO 给出 m/s^2 与 rad/s，界面和 CSV 沿用 g 与 dps */
static const double kStandardGravity = 9.80665;
static const double kRadToDeg = 57.29577951308232;

void MainWindow::updateIcmData(const QVector<ImuSample> &samples, double rateHz)
{
	/* 界面显示这个刷新周期内全部样本的平均值，比单点读数更稳 */
	double accel[3] = { 0, 0, 0 };
	double gyro[3] = { 0, 0, 0 };
	double temperature = 0;
	for (const ImuSample &sample : samples) {
		for (int axis = 0; axis < 3; axis++) {
			accel[axis] += sample.accel[axis];
			gyro[axis] += sample.gyro[axis];
		}
		temperature += sample.tempC;
	}
	const double n = samples.size();
	for (int axis = 0; axis < 3; axis++) {
		accel[axis] /= n * kStandardGravity;
		gyro[axis] *= kRadToDeg / n;
	}
	temperature /= n;

	ui->icmStatusLabel->setText(QString("ICM20608: OK  IIO %1 Hz").arg(rateHz, 0, 'f', 1));
	ui->icmStatusLabel->setStyleSheet("color: #89d185;");
	ui->icmStatusLabel->setToolTip(
		QString("%1 samples in this interval (average shown)").arg(samples.size()));

	ui->accelLabel->setText(
		QString("ACC(g): X=%1 Y=%2 Z=%3")
			.arg(accel[0], 0, 'f', 3)
			.arg(accel[1], 0, 'f', 3)
			.arg(accel[2], 0, 'f', 3));

	ui->gyroLabel->setText(
		QString("GYRO(dps): X=%1 Y=%2 Z=%3")
			.arg(gyro[0], 0, 'f', 2)
			.arg(gyro[1], 0, 'f', 2)
			.arg(gyro[2], 0, 'f', 2));

	ui->tempLabel->setText(QString("TEMP: %1 C").arg(temperature, 0, 'f', 2));

	if (recording) {
		/* 每个样本一行，攒成一次写入，避免每行一次系统调用 */
		QByteArray rows;
		rows.reserve(samples.size() * 96);
		for (const ImuSample &sample : samples) {
			/* 这一批从上个刷新周期开始攒，跳过按下 Start 之前的样本 */
			if (sample.timestampNs < recordStartNs) {
				continue;
			}
			const QDateTime time = QDateTime::fromMSecsSinceEpoch(sample.timestampNs / 1000000);
			rows += QString("%1,%2,%3,%4,%5,%6,%7,%8,%9\n")
				.arg(time.toString(Qt::ISODateWithMs))
				.arg(sample.timestampNs)
				.arg(sample.accel[0] / kStandardGravity, 0, 'f', 4)
				.arg(sample.accel[1] / kStandardGravity, 0, 'f', 4)
				.arg(sample.accel[2] / kStandardGravity, 0, 'f', 4)
				.arg(sample.gyro[0] * kRadToDeg, 0, 'f', 3)
				.arg(sample.gyro[1] * kRadToDeg, 0, 'f', 3)
				.arg(sample.gyro[2] * kRadToDeg, 0, 'f', 3)
				.arg(sample.tempC, 0, 'f', 2)
				.toUtf8();
		}
		appendRecord(icmRecordFile, rows);
	}
}

void MainWindow::showIcmError(const QString &message)
{
	qWarning() << message;

	ui->icmStatusLabel->setText("ICM20608: ERROR");
	ui->icmStatusLabel->setStyleSheet("color: #f48771;");
	ui->icmStatusLabel->setToolTip(message);

	ui->accelLabel->setText("ACC: --");
	ui->gyroLabel->setText("GYRO: --");
	ui->tempLabel->setText("TEMP: --");

	ui->statusbar->showMessage(message, 5000);
}

void MainWindow::loadSettings()
{
	QString path = QCoreApplication::applicationDirPath()
		+ "/device_monitor.ini";
	QSettings settings(path, QSettings::IniFormat);
	bool onOk;
	bool offOk;
	bool intervalOk;
	int onThreshold = settings.value("led/onThreshold", 20).toInt(&onOk);
	int offThreshold = settings.value("led/offThreshold", 50).toInt(&offOk);
	int interval = settings.value("sensor/intervalMs", 1000).toInt(&intervalOk);

	if (settings.status() == QSettings::NoError && onOk && offOk && intervalOk
		&& onThreshold >= 0 && offThreshold <= 65535
		&& onThreshold < offThreshold && interval >= 200 && interval <= 60000) {
		lightOnThreshold = onThreshold;
		lightOffThreshold = offThreshold;
		sampleIntervalMs = interval;
		ui->settingsFeedbackLabel->setText("Parameters loaded. Startup mode: manual.");
	} else {
		ui->settingsFeedbackLabel->setText(
			"Invalid or unreadable configuration. Using defaults: 20 / 50 / 1000 ms.");
		ui->settingsFeedbackLabel->setStyleSheet("color: #dcdcaa;");
	}

	ui->lightOnSpinBox->setValue(lightOnThreshold);
	ui->lightOffSpinBox->setValue(lightOffThreshold);
	ui->sampleIntervalSpinBox->setValue(sampleIntervalMs);
	ui->configPathLabel->setText("Config: " + path);

	/* IMU 采样率只在 ini 中配置（界面不提供），重启程序生效 */
	bool rateOk;
	int rate = settings.value("sensor/imuRateHz", 100).toInt(&rateOk);
	if (rateOk && rate >= 4 && rate <= 1000) {
		imuRateHz = rate;
	}
}

void MainWindow::on_applySettingsButton_clicked()
{
	int onThreshold = ui->lightOnSpinBox->value();
	int offThreshold = ui->lightOffSpinBox->value();
	int interval = ui->sampleIntervalSpinBox->value();

	if (onThreshold >= offThreshold) {
		ui->settingsFeedbackLabel->setText(
			"Not applied: LED ON threshold must be lower than LED OFF threshold.");
		ui->settingsFeedbackLabel->setStyleSheet("color: #f48771;");
		return;
	}

	lightOnThreshold = onThreshold;
	lightOffThreshold = offThreshold;
	sampleIntervalMs = interval;
	emit requestSampleInterval(sampleIntervalMs);

	QString path = QCoreApplication::applicationDirPath()
		+ "/device_monitor.ini";
	QSettings settings(path, QSettings::IniFormat);

	settings.setValue("led/onThreshold", lightOnThreshold);
	settings.setValue("led/offThreshold", lightOffThreshold);
	settings.setValue("sensor/intervalMs", sampleIntervalMs);
	settings.sync();

	if (settings.status() != QSettings::NoError) {
		ui->settingsFeedbackLabel->setText(
			"Applied for this run, but saving failed. Check the config directory permissions.");
		ui->settingsFeedbackLabel->setStyleSheet("color: #f48771;");
		return;
	}

	ui->settingsFeedbackLabel->setText(
		"Applied and saved. Auto LED uses the next valid ALS sample.");
	ui->settingsFeedbackLabel->setStyleSheet("color: #89d185;");
	ui->statusbar->showMessage("Settings applied and saved", 5000);
}

void MainWindow::on_recordButton_clicked()
{
	if (recording) {
		stopRecording();
	} else {
		startRecording();
	}
}

void MainWindow::startRecording()
{
	QString directory = QCoreApplication::applicationDirPath() + "/records";
	QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");

	ui->recordPathLabel->setText("Path: " + directory);

	if (!QDir().mkpath(directory)) {
		stopRecording("Cannot create records directory.");
		return;
	}

	apRecordFile.setFileName(directory + "/ap3216c_" + stamp + ".csv");
	icmRecordFile.setFileName(directory + "/icm20608_" + stamp + ".csv");

	/* 每次创建新文件，已有同名记录不会被覆盖。 */
	if (!apRecordFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
		stopRecording("Open AP CSV failed: " + apRecordFile.errorString());
		return;
	}

	if (!icmRecordFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
		QString error = "Open ICM CSV failed: " + icmRecordFile.errorString();

		apRecordFile.close();
		apRecordFile.remove();
		stopRecording(error);
		return;
	}

	QByteArray apHeader = "timestamp,ir_raw,als_raw,als_lux,ps_raw,flags\n";
	/* ICM 每个 IIO 样本一行，时间来自内核在数据就绪中断里记录的时间戳 */
	QByteArray icmHeader = "timestamp,kernel_timestamp_ns,accel_x_g,accel_y_g,accel_z_g,"
		"gyro_x_dps,gyro_y_dps,gyro_z_dps,temp_c\n";

	if (apRecordFile.write(apHeader) != apHeader.size()
		|| icmRecordFile.write(icmHeader) != icmHeader.size()
		|| !apRecordFile.flush() || !icmRecordFile.flush()) {
		stopRecording("Write CSV header failed. Check storage space and permissions.");
		apRecordFile.remove();
		icmRecordFile.remove();
		return;
	}

	recordStartNs = QDateTime::currentMSecsSinceEpoch() * 1000000;
	recording = true;
	ui->recordStatusLabel->setToolTip(QString());
	ui->recordButton->setText("Stop Recording");
	ui->recordStatusLabel->setText("Recording: ON");
	ui->recordStatusLabel->setStyleSheet("color: #89d185;");
	ui->statusbar->showMessage("Recording started", 5000);
}

void MainWindow::stopRecording(const QString &error)
{
	QString message = error;

	for (QFile *file : { &apRecordFile, &icmRecordFile }) {
		if (file->isOpen()) {
			if (!file->flush() && message.isEmpty()) {
				message = "Flush CSV failed: " + file->errorString();
			}

			file->close();
		}
	}

	recording = false;
	ui->recordButton->setText("Start Recording");

	if (!message.isEmpty()) {
		qWarning() << message;
		ui->recordStatusLabel->setText("Recording: ERROR");
		ui->recordStatusLabel->setStyleSheet("color: #f48771;");
		ui->recordStatusLabel->setToolTip(message);
		ui->statusbar->showMessage(message, 5000);
		return;
	}

	ui->recordStatusLabel->setText("Recording: OFF");
	ui->recordStatusLabel->setStyleSheet("color: #d4d4d4;");
	ui->recordStatusLabel->setToolTip(QString());
}

void MainWindow::appendRecord(QFile &file, const QByteArray &row)
{
	/* 小规模记录直接写文件；每行刷新 Qt 缓冲区，及时发现写入错误。 */
	if (file.write(row) != row.size() || !file.flush()) {
		stopRecording("Write CSV failed: " + file.errorString());
	}
}
