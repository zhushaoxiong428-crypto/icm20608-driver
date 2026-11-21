#ifndef SYSTEMPAGE_H
#define SYSTEMPAGE_H

#include <QHash>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;

/* 最近若干个采集周期的 CPU 占用和 SoC 温度曲线，QPainter 直接画，不依赖 QtCharts */
class HistoryChart : public QWidget
{
	Q_OBJECT

public:
	explicit HistoryChart(QWidget *parent = nullptr);
	void append(double cpuPct, double tempC);
	void clear();

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	static const int kMaxPoints = 120;
	QVector<double> cpu;
	QVector<double> temp;
};

/*
 * System 标签页：显示 devmond 推送的 SYS 消息（daemon/protocol.md）。
 * 用代码创建并插入 QTabWidget，三套 .ui 布局都不用改。
 */
class SystemPage : public QWidget
{
	Q_OBJECT

public:
	explicit SystemPage(QWidget *parent = nullptr);
	void updateStats(const QHash<QString, QString> &fields);
	void updateMqtt(const QHash<QString, QString> &fields);	/* MQTT state= broker= msg= */
	void setOffline();

private:
	QLabel *cpuLabel;
	QLabel *memLabel;
	QLabel *loadLabel;
	QLabel *tempLabel;
	QLabel *netLabel;
	QLabel *diskLabel;
	QLabel *timeLabel;
	QLabel *mqttLabel;
	HistoryChart *chart;
};

#endif /* SYSTEMPAGE_H */
