#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QHash>
#include <QMainWindow>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class DaemonClient;
class SystemPage;

/*
 * 界面只做显示和下发命令：采集、LED、按键、自动亮灯、参数保存、CSV 录制都在 devmond 里，
 * 界面退出不影响采集和录制；守护进程推送什么状态，界面就显示什么。
 */
class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow(QWidget *parent = nullptr);
	~MainWindow();

private slots:
	void on_pushButton_clicked();
	void on_exitButton_clicked();
	void on_readSensorButton_clicked();
	void on_autoModeCheckBox_toggled(bool checked);
	void on_applySettingsButton_clicked();
	void on_recordButton_clicked();

	void onConnectionChanged(bool connected, const QString &reason);
	void onMessage(const QString &type, const QHash<QString, QString> &fields);

private:
	typedef QHash<QString, QString> Fields;

	void updateConfig(const Fields &f);
	void updateLed(const Fields &f);
	void updateKey(const Fields &f);
	void updateRecord(const Fields &f);
	void updateAp(const Fields &f);
	void updateProximity(const Fields &f);
	void updateImu(const Fields &f);
	void showApError(const QString &message);
	void showImuError(const QString &message);
	void handleReply(bool ok, const Fields &f);
	void setControlsEnabled(bool enabled);

	Ui::MainWindow *ui;
	DaemonClient *client;
	SystemPage *systemPage;

	bool ledAvailable;
	bool autoMode;
	bool recording;
	int proximityState; /* -1 未知，0 远，1 近 */
};

#endif /* MAINWINDOW_H */
