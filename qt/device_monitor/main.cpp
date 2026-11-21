#include "mainwindow.h"

#include <QApplication>
#include <QFont>

int main(int argc, char *argv[])
{
	QApplication a(argc, argv); /* 准备 Qt 应用环境 */

	/*
	 * 界面样式表指定 Consolas（Windows 字体，不随源码分发）；开发板上用开源等宽字体
	 * DejaVu Sans Mono 代替（放在 QT_QPA_FONTDIR 下），保证数字列对齐
	 */
	QFont::insertSubstitution("Consolas", "DejaVu Sans Mono");

	MainWindow w;
	w.showFullScreen(); /* 显示屏幕，通过继承父类的成员函数 */

	return a.exec(); /* 阻塞等待输入、定时器或其他通知 */
}
