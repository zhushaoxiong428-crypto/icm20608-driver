// 离屏预览 mainwindow.ui：按指定分辨率渲染每个标签页为 PNG
#include <QApplication>
#include <QMainWindow>
#include <QTabWidget>
#include <QPixmap>
#include <QLabel>
#include <QAbstractButton>
#include "ui_mainwindow.h"
int main(int argc, char **argv) {
    QApplication a(argc, argv);
    int w = argc > 1 ? atoi(argv[1]) : 800, h = argc > 2 ? atoi(argv[2]) : 480;
    QString prefix = argc > 3 ? argv[3] : "shot";
    QMainWindow mw;
    Ui::MainWindow ui;
    ui.setupUi(&mw);
    ui.statusbar->showMessage("Settings applied and saved");
    // 用运行时最长的文字填充，检查宽度
    const char *fill[][2] = {
        {"apStatusLabel", "AP3216C: OK"}, {"irLabel", "IR: 1023"}, {"alsLabel", "ALS: 65535"}, {"psLabel", "PS: 1023"},
        {"icmStatusLabel", "ICM20608: OK"}, {"accelLabel", "ACC(g): X=-0.012 Y=-0.003 Z=-1.001"},
        {"gyroLabel", "GYRO(dps): X=-123.45 Y=-1999.99 Z=-1.23"}, {"tempLabel", "TEMP: 31.25 C"},
        {"keyLabel", "KEY: RELEASED"}, {"pushButton", "LED OFF - Turn ON"},
        {"settingsFeedbackLabel", "Invalid: LED ON must be lower than LED OFF. Using defaults."},
        {"configPathLabel", "Config: /device-management/device_monitor.ini"},
        {"recordStatusLabel", "Recording: ON"}, {"recordButton", "Stop Recording"},
        {"recordPathLabel", "Path: /device-management/records"},
    };
    for (auto &f : fill) {
        if (QLabel *l = mw.findChild<QLabel *>(f[0])) l->setText(f[1]);
        else if (QAbstractButton *b = mw.findChild<QAbstractButton *>(f[0])) b->setText(f[1]);
    }
    mw.setFixedSize(w, h);
    mw.show();
    for (int i = 0; i < ui.mainTabWidget->count(); ++i) {
        ui.mainTabWidget->setCurrentIndex(i);
        a.processEvents();
        mw.grab().save(QString("%1_tab%2.png").arg(prefix).arg(i));
    }
    // 报告被挤压到小于 sizeHint 的控件（布局放不下的信号）
    for (QWidget *c : mw.findChildren<QWidget *>()) {
        if (!c->isVisible() || c->objectName().isEmpty() || c->objectName().startsWith("qt_")) continue;
        QSize m = c->minimumSizeHint();
        if (c->height() < m.height() || c->width() < m.width())
            qWarning("squeezed: %s %dx%d < hint %dx%d", qPrintable(c->objectName()), c->width(), c->height(), m.width(), m.height());
    }
    return 0;
}
