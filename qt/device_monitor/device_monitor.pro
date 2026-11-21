QT       += core gui

INCLUDEPATH += $$PWD/../../include

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    ap3216ciio.cpp \
    icm20608iio.cpp \
    main.cpp \
    mainwindow.cpp \
    sensorworker.cpp

HEADERS += \
    ap3216ciio.h \
    icm20608iio.h \
    mainwindow.h \
    sensorworker.h

# 屏幕布局：默认 ATK-4384（4.3 寸 800x480）；qmake LCD=480x272（ATK-4342）或 LCD=1024x600（7 寸）
# 各 .ui 的控件 objectName 相同，uic 都生成 ui_mainwindow.h，C++ 代码无需改动
# 小屏布局由 gen_ui.py 生成：python3 gen_ui.py 800x480
isEmpty(LCD): LCD = 800x480
equals(LCD, 1024x600) {
    FORMS += mainwindow.ui
} else:equals(LCD, 800x480) {
    FORMS += ui_800x480/mainwindow.ui
} else:equals(LCD, 480x272) {
    FORMS += ui_480x272/mainwindow.ui
} else {
    error("Unsupported LCD=$$LCD, use 800x480, 480x272 or 1024x600")
}
message("device_monitor UI layout: $$LCD")

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
