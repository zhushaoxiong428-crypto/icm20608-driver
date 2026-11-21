#include "systempage.h"

#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

/* 与界面样式表的配色一致 */
static const QColor kCpuColor(0x4f, 0xc1, 0xff);
static const QColor kTempColor(0xdc, 0xdc, 0xaa);
static const QColor kGridColor(0x45, 0x45, 0x45);
static const QColor kTextColor(0x99, 0x99, 0x99);
static const char *const kNormal = "color: #d4d4d4;";
static const char *const kAlarm = "color: #f48771;";

/* ---------------- 曲线 ---------------- */

HistoryChart::HistoryChart(QWidget *parent)
	: QWidget(parent)
{
	setMinimumHeight(60);
}

void HistoryChart::append(double cpuPct, double tempC)
{
	cpu.append(cpuPct);
	temp.append(tempC);
	if (cpu.size() > kMaxPoints) {
		cpu.removeFirst();
		temp.removeFirst();
	}
	update();
}

void HistoryChart::clear()
{
	cpu.clear();
	temp.clear();
	update();
}

void HistoryChart::paintEvent(QPaintEvent *event)
{
	Q_UNUSED(event);
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	const QFontMetrics fm = p.fontMetrics();
	const int legend = fm.height() + 4;
	const QRectF plot(fm.horizontalAdvance("100") + 6, legend, width() - fm.horizontalAdvance("100") - 10,
		height() - legend - 4);

	/* 纵轴 0~100：CPU 为 %，温度为 °C（SoC 正常工作在这个范围内） */
	p.setPen(kGridColor);
	for (int v = 0; v <= 100; v += 25) {
		const qreal y = plot.bottom() - plot.height() * v / 100.0;
		p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
		p.setPen(kTextColor);
		p.drawText(QRectF(0, y - fm.height() / 2.0, plot.left() - 4, fm.height()),
			Qt::AlignRight | Qt::AlignVCenter, QString::number(v));
		p.setPen(kGridColor);
	}

	const qreal tempX = plot.left() + fm.horizontalAdvance("CPU %") + 16;
	const QString span = QString("last %1 samples").arg(kMaxPoints);
	p.setPen(kCpuColor);
	p.drawText(QPointF(plot.left(), fm.ascent()), "CPU %");
	p.setPen(kTempColor);
	p.drawText(QPointF(tempX, fm.ascent()), "SoC C");
	/* 窄屏时放不下就不画，避免与图例重叠 */
	if (tempX + fm.horizontalAdvance("SoC C") + 16 + fm.horizontalAdvance(span) <= plot.right()) {
		p.setPen(kTextColor);
		p.drawText(QRectF(plot.left(), 0, plot.width(), legend), Qt::AlignRight, span);
	}

	const auto drawSeries = [&](const QVector<double> &data, const QColor &color) {
		if (data.size() < 2) {
			return;
		}
		QPolygonF line;
		/* 最新的点在最右边，未满时从右向左排 */
		const qreal step = plot.width() / (kMaxPoints - 1);
		const qreal x0 = plot.right() - step * (data.size() - 1);
		for (int i = 0; i < data.size(); i++) {
			const qreal v = qBound(0.0, data[i], 100.0);
			line << QPointF(x0 + step * i, plot.bottom() - plot.height() * v / 100.0);
		}
		p.setPen(QPen(color, 2));
		p.drawPolyline(line);
	};
	drawSeries(temp, kTempColor);
	drawSeries(cpu, kCpuColor);
}

/* ---------------- System 页 ---------------- */

static QString formatUptime(long seconds)
{
	const long days = seconds / 86400;
	const long hours = seconds % 86400 / 3600;
	const long minutes = seconds % 3600 / 60;
	if (days > 0) {
		return QString("%1d %2h").arg(days).arg(hours);
	}
	return hours > 0 ? QString("%1h %2m").arg(hours).arg(minutes)
			 : QString("%1m %2s").arg(minutes).arg(seconds % 60);
}

static QString formatSize(double mb)
{
	return mb >= 1024 ? QString("%1 GB").arg(mb / 1024, 0, 'f', 1)
			  : QString("%1 MB").arg(mb, 0, 'f', 0);
}

SystemPage::SystemPage(QWidget *parent)
	: QWidget(parent)
	, cpuLabel(new QLabel)
	, memLabel(new QLabel)
	, loadLabel(new QLabel)
	, tempLabel(new QLabel)
	, netLabel(new QLabel)
	, diskLabel(new QLabel)
	, timeLabel(new QLabel)
	, chart(new HistoryChart)
{
	/* 与其他标签页同样的深色背景（普通 QWidget 需要 WA_StyledBackground 才会画样式表背景） */
	setObjectName("systemPage");
	setAttribute(Qt::WA_StyledBackground, true);
	setStyleSheet("QWidget#systemPage { background-color: #1e1e1e; }");

	QGroupBox *stats = new QGroupBox("Resources");
	QVBoxLayout *statsLayout = new QVBoxLayout(stats);
	for (QLabel *label : { timeLabel, cpuLabel, memLabel, loadLabel, tempLabel, netLabel, diskLabel }) {
		statsLayout->addWidget(label);
	}
	statsLayout->addStretch(1);

	QGroupBox *history = new QGroupBox("History");
	QVBoxLayout *historyLayout = new QVBoxLayout(history);
	historyLayout->addWidget(chart);

	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(4, 3, 4, 4);
	layout->addWidget(stats, 1);
	layout->addWidget(history, 1);

	setOffline();
}

void SystemPage::setOffline()
{
	cpuLabel->setText("CPU: --");
	memLabel->setText("MEM: --");
	loadLabel->setText("LOAD: --");
	tempLabel->setText("SoC: --");
	netLabel->setText("NET: --");
	diskLabel->setText("DISK: --");
	timeLabel->setText("TIME: --");
	timeLabel->setStyleSheet(kNormal);
	chart->clear();
}

void SystemPage::updateStats(const QHash<QString, QString> &f)
{
	const double cpu = f.value("cpu").toDouble();
	const double memUsed = f.value("mem_used").toDouble();
	const double memTotal = f.value("mem_total").toDouble();
	const double temp = f.value("temp").toDouble();
	const double diskFree = f.value("disk_free").toDouble();
	const double diskTotal = f.value("disk_total").toDouble();
	const bool tempOk = temp > -273.0;

	cpuLabel->setText(QString("CPU: %1 %  (devmond %2 %)")
		.arg(cpu, 0, 'f', 1).arg(f.value("self_cpu").toDouble(), 0, 'f', 2));
	memLabel->setText(QString("MEM: %1 / %2 MB (%3 %)")
		.arg(memUsed, 0, 'f', 0).arg(memTotal, 0, 'f', 0)
		.arg(memTotal > 0 ? 100.0 * memUsed / memTotal : 0.0, 0, 'f', 0));
	loadLabel->setText(QString("LOAD: %1  UP: %2")
		.arg(f.value("load"), formatUptime(f.value("uptime").toLong())));

	tempLabel->setText(tempOk ? QString("SoC: %1 C").arg(temp, 0, 'f', 1) : QString("SoC: n/a"));
	tempLabel->setStyleSheet(tempOk && temp >= 85.0 ? kAlarm : kNormal);

	netLabel->setText(QString("NET: rx %1  tx %2 KB/s")
		.arg(f.value("rx").toDouble(), 0, 'f', 1).arg(f.value("tx").toDouble(), 0, 'f', 1));

	if (diskTotal > 0) {
		diskLabel->setText(QString("DISK: %1 free / %2").arg(formatSize(diskFree), formatSize(diskTotal)));
		/* 录制目录所在文件系统快满时提示，避免录制写盘失败 */
		diskLabel->setStyleSheet(diskFree < 100 ? kAlarm : kNormal);
	} else {
		diskLabel->setText("DISK: n/a");
	}

	/* 板子的系统时间（远程连接时按本机时区显示）；未经 NTP 校准时标黄提醒 */
	const bool synced = f.value("ntp") == "1";
	timeLabel->setText(QString("TIME: %1  %2")
		.arg(QDateTime::fromMSecsSinceEpoch(f.value("time").toLongLong()).toString("yyyy-MM-dd HH:mm:ss"),
			synced ? "NTP ok" : "not synced"));
	timeLabel->setStyleSheet(synced ? kNormal : "color: #dcdcaa;");

	chart->append(cpu, tempOk ? temp : 0.0);
}
