#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ap3216c_data.h"
#include "daemonclient.h"
#include "systempage.h"

#include <QDateTime>
#include <QDebug>
#include <QRegExp>
#include <QSignalBlocker>

static const char *const kOk = "color: #89d185;";
static const char *const kWarn = "color: #dcdcaa;";
static const char *const kError = "color: #f48771;";

MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent)
	, ui(new Ui::MainWindow)
	, client(new DaemonClient(this))
	, systemPage(new SystemPage)
	, ledAvailable(false)
	, autoMode(false)
	, recording(false)
	, proximityState(-1)
{
	ui->setupUi(this);

	/* System 页用代码插入，三套 .ui 布局都不用改 */
	ui->mainTabWidget->insertTab(1, systemPage, "System");

	ui->apStatusLabel->setText("AP3216C: WAITING");
	ui->icmStatusLabel->setText("ICM20608: WAITING");
	ui->keyLabel->setText("KEY: --");
	ui->settingsFeedbackLabel->setText("Waiting for devmond...");
	setControlsEnabled(false);

	connect(client, &DaemonClient::connectionChanged, this, &MainWindow::onConnectionChanged);
	connect(client, &DaemonClient::message, this, &MainWindow::onMessage);
	client->start();
}

MainWindow::~MainWindow()
{
	/* LED、录制等状态属于守护进程，界面退出不做改动 */
	delete ui;
}

void MainWindow::setControlsEnabled(bool enabled)
{
	ui->pushButton->setEnabled(enabled && ledAvailable && !autoMode);
	ui->autoModeCheckBox->setEnabled(enabled && ledAvailable);
	ui->readSensorButton->setEnabled(enabled);
	ui->applySettingsButton->setEnabled(enabled);
	ui->recordButton->setEnabled(enabled);
}

/* ---------------- 连接与消息分发 ---------------- */

void MainWindow::onConnectionChanged(bool connected, const QString &reason)
{
	setControlsEnabled(connected);
	if (connected) {
		ui->statusbar->showMessage("Connected to devmond at " + client->address(), 3000);
		return;
	}

	qWarning().noquote() << reason;
	ui->apStatusLabel->setText("AP3216C: WAITING");
	ui->apStatusLabel->setStyleSheet(kWarn);
	ui->icmStatusLabel->setText("ICM20608: WAITING");
	ui->icmStatusLabel->setStyleSheet(kWarn);
	ui->keyLabel->setText("KEY: --");
	systemPage->setOffline();
	ui->statusbar->showMessage(reason + " - retrying");
}

void MainWindow::onMessage(const QString &type, const QHash<QString, QString> &fields)
{
	if (type == "AP") {
		updateAp(fields);
	} else if (type == "IMU") {
		updateImu(fields);
	} else if (type == "SYS") {
		systemPage->updateStats(fields);
	} else if (type == "PROX") {
		updateProximity(fields);
	} else if (type == "LED") {
		updateLed(fields);
	} else if (type == "KEY") {
		updateKey(fields);
	} else if (type == "CONFIG") {
		updateConfig(fields);
	} else if (type == "REC") {
		updateRecord(fields);
	} else if (type == "AP_ERR") {
		showApError(fields.value("msg"));
	} else if (type == "IMU_ERR") {
		showImuError(fields.value("msg"));
	} else if (type == "OK" || type == "ERR") {
		handleReply(type == "OK", fields);
	}
}

/* ---------------- 状态 ---------------- */

void MainWindow::updateConfig(const Fields &f)
{
	ui->lightOnSpinBox->setValue(f.value("on").toInt());
	ui->lightOffSpinBox->setValue(f.value("off").toInt());
	ui->sampleIntervalSpinBox->setValue(f.value("interval").toInt());
	ui->configPathLabel->setText(QString("Config: %1 (devmond, IMU %2 Hz)")
		.arg(f.value("path"), f.value("imu_rate")));
	if (ui->settingsFeedbackLabel->text().startsWith("Waiting")) {
		ui->settingsFeedbackLabel->setText("Parameters loaded from devmond. Startup mode: manual.");
	}
}

void MainWindow::updateLed(const Fields &f)
{
	const QString state = f.value("state");
	const bool newAuto = f.value("auto") == "1";

	ledAvailable = state != "unavailable";
	if (newAuto != autoMode) {
		ui->statusbar->showMessage(newAuto ? "Auto mode: waiting for valid ALS data"
						   : "Manual mode", 5000);
	}
	autoMode = newAuto;

	{
		/* 来自守护进程的状态同步，不要再触发 toggled 槽发命令 */
		const QSignalBlocker blocker(ui->autoModeCheckBox);
		ui->autoModeCheckBox->setChecked(autoMode);
	}
	ui->pushButton->setText(!ledAvailable ? "LED unavailable"
				: state == "on" ? "LED ON - Turn OFF" : "LED OFF - Turn ON");
	setControlsEnabled(client->isConnected());
}

void MainWindow::updateKey(const Fields &f)
{
	const QString state = f.value("state");
	ui->keyLabel->setText(state == "unavailable" ? "KEY: unavailable" : "KEY: " + state.toUpper());
}

void MainWindow::updateRecord(const Fields &f)
{
	const QString state = f.value("state");

	recording = state == "on";
	ui->recordButton->setText(recording ? "Stop Recording" : "Start Recording");
	if (f.contains("dir")) {
		ui->recordPathLabel->setText("Path: " + f.value("dir"));
	}

	if (state == "error") {
		const QString message = f.value("msg");
		qWarning().noquote() << message;
		ui->recordStatusLabel->setText("Recording: ERROR");
		ui->recordStatusLabel->setStyleSheet(kError);
		ui->recordStatusLabel->setToolTip(message);
		ui->statusbar->showMessage(message, 5000);
		return;
	}

	ui->recordStatusLabel->setText(recording ? "Recording: ON" : "Recording: OFF");
	ui->recordStatusLabel->setStyleSheet(recording ? kOk : "color: #d4d4d4;");
	ui->recordStatusLabel->setToolTip(QString());
}

void MainWindow::updateAp(const Fields &f)
{
	/* 接近状态来自中断事件（实时），PS 数值按采集间隔刷新 */
	static const char *const kProximityText[] = { "", "  FAR", "  NEAR" };
	const QString proximity = kProximityText[proximityState + 1];
	const int flags = f.value("flags").toInt();

	ui->apStatusLabel->setText("AP3216C: OK");
	ui->apStatusLabel->setStyleSheet(kOk);
	ui->apStatusLabel->setToolTip(QString());

	ui->irLabel->setText((flags & AP3216C_IR_VALID) ? "IR: " + f.value("ir") : "IR: invalid");
	ui->alsLabel->setText((flags & AP3216C_ALS_VALID)
		? QString("ALS: %1 (%2 lx)").arg(f.value("als")).arg(f.value("lux").toDouble(), 0, 'f', 1)
		: QString("ALS: invalid"));
	ui->psLabel->setText(((flags & AP3216C_PS_VALID) ? "PS: " + f.value("ps") : "PS: invalid")
		+ proximity);
}

void MainWindow::updateProximity(const Fields &f)
{
	const bool near = f.value("state") == "near";

	proximityState = near ? 1 : 0;

	/* 不等下一次采集，立即更新 PS 一栏的状态后缀 */
	QString text = ui->psLabel->text();
	text.remove(QRegExp("  (NEAR|FAR)$"));
	ui->psLabel->setText(text + (near ? "  NEAR" : "  FAR"));

	/* 新连上时守护进程补发的当前状态不带 ts，不算一次新事件 */
	if (f.contains("ts")) {
		const QDateTime time = QDateTime::fromMSecsSinceEpoch(f.value("ts").toLongLong() / 1000000);
		ui->statusbar->showMessage(QString("Proximity %1 (interrupt #%2 at %3)")
			.arg(near ? "NEAR" : "FAR", f.value("count"), time.toString("HH:mm:ss.zzz")), 5000);
	}
}

void MainWindow::updateImu(const Fields &f)
{
	ui->icmStatusLabel->setText(QString("ICM20608: OK  IIO %1 Hz").arg(f.value("rate")));
	ui->icmStatusLabel->setStyleSheet(kOk);
	ui->icmStatusLabel->setToolTip(QString("%1 samples in this interval (average shown)")
		.arg(f.value("n")));

	ui->accelLabel->setText(QString("ACC(g): X=%1 Y=%2 Z=%3")
		.arg(f.value("ax"), f.value("ay"), f.value("az")));
	ui->gyroLabel->setText(QString("GYRO(dps): X=%1 Y=%2 Z=%3")
		.arg(f.value("gx"), f.value("gy"), f.value("gz")));
	ui->tempLabel->setText(QString("TEMP: %1 C").arg(f.value("temp")));
}

void MainWindow::showApError(const QString &message)
{
	qWarning().noquote() << message;
	ui->apStatusLabel->setText("AP3216C: ERROR");
	ui->apStatusLabel->setStyleSheet(kError);
	ui->apStatusLabel->setToolTip(message);
	ui->irLabel->setText("IR: --");
	ui->alsLabel->setText("ALS: --");
	ui->psLabel->setText("PS: --");
	ui->statusbar->showMessage(message, 5000);
}

void MainWindow::showImuError(const QString &message)
{
	qWarning().noquote() << message;
	ui->icmStatusLabel->setText("ICM20608: ERROR");
	ui->icmStatusLabel->setStyleSheet(kError);
	ui->icmStatusLabel->setToolTip(message);
	ui->accelLabel->setText("ACC: --");
	ui->gyroLabel->setText("GYRO: --");
	ui->tempLabel->setText("TEMP: --");
	ui->statusbar->showMessage(message, 5000);
}

/* ---------------- 命令应答 ---------------- */

void MainWindow::handleReply(bool ok, const Fields &f)
{
	const QString cmd = f.value("cmd");
	const QString message = f.value("msg");

	if (cmd == "CONFIG") {
		if (ok) {
			ui->settingsFeedbackLabel->setText(
				"Applied and saved. Auto LED uses the next valid ALS sample.");
			ui->settingsFeedbackLabel->setStyleSheet(kOk);
			ui->statusbar->showMessage("Settings applied and saved", 5000);
		} else {
			ui->settingsFeedbackLabel->setText("Not applied: " + message);
			ui->settingsFeedbackLabel->setStyleSheet(kError);
		}
		return;
	}
	if (ok) {
		return;
	}

	qWarning().noquote() << "devmond:" << cmd << message;
	if (cmd == "AUTO") {
		/* 守护进程拒绝了，复选框恢复到守护进程的真实状态 */
		const QSignalBlocker blocker(ui->autoModeCheckBox);
		ui->autoModeCheckBox->setChecked(autoMode);
	} else if (cmd == "REC") {
		ui->recordStatusLabel->setText("Recording: ERROR");
		ui->recordStatusLabel->setStyleSheet(kError);
		ui->recordStatusLabel->setToolTip(message);
	}
	ui->statusbar->showMessage(cmd + ": " + message, 5000);
}

/* ---------------- 界面操作 -> 命令 ---------------- */

void MainWindow::on_pushButton_clicked()
{
	client->send("LED toggle");
}

void MainWindow::on_exitButton_clicked()
{
	close(); /* 只关界面，采集和录制在守护进程里继续 */
}

void MainWindow::on_readSensorButton_clicked()
{
	client->send("READ");
}

void MainWindow::on_autoModeCheckBox_toggled(bool checked)
{
	client->send(checked ? "AUTO on" : "AUTO off");
}

void MainWindow::on_applySettingsButton_clicked()
{
	const int onThreshold = ui->lightOnSpinBox->value();
	const int offThreshold = ui->lightOffSpinBox->value();

	if (onThreshold >= offThreshold) {
		ui->settingsFeedbackLabel->setText(
			"Not applied: LED ON threshold must be lower than LED OFF threshold.");
		ui->settingsFeedbackLabel->setStyleSheet(kError);
		return;
	}

	client->send(QString("CONFIG on=%1 off=%2 interval=%3")
		.arg(onThreshold).arg(offThreshold).arg(ui->sampleIntervalSpinBox->value()));
}

void MainWindow::on_recordButton_clicked()
{
	client->send(recording ? "REC stop" : "REC start");
}
