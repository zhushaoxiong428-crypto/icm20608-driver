#!/bin/bash
# 在 PC 上离屏渲染 mainwindow.ui，不需要开发板和 ARM Qt（需要主机装 qtbase5-dev）
# 用法：run.sh <ui文件> <输出前缀> [宽] [高]
#   例：qt/ui_preview/run.sh qt/device_monitor/ui_800x480/mainwindow.ui /tmp/shot 800 480
# 输出 <前缀>_tab0.png、_tab1.png …；被挤压到小于最小尺寸的控件会打印 squeezed 警告
set -e
P=$(dirname "$(readlink -f "$0")")
UI=$(readlink -f "$1")
OUT=$(readlink -f "$2")
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
cp "$UI" "$B/mainwindow.ui"
cd "$B"
qmake "$P/preview.pro" UI_FILE="$B/mainwindow.ui" >/dev/null
make -s
QT_QPA_PLATFORM=offscreen ./preview "${3:-800}" "${4:-480}" "$OUT"
