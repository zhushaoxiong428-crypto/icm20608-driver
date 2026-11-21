#!/bin/sh
# 开发板上启动 device_monitor：加载驱动 → 按设备名找 input 节点 → 设置 tslib/Qt 环境变量 → 运行
# 用法：./run_device_monitor.sh            触摸走 tslib（正点原子手册方式）
#       TOUCH=evdev ./run_device_monitor.sh  触摸走 Qt 自带 evdevtouch（支持多点，不需要 tslib）
# 约定路径（与正点原子《移植Qt5.12.9》一致）：Qt 在 /usr/lib/arm-qt，tslib 在 /usr/lib/arm-tslib，
# 字体在 /usr/share/fonts；本脚本、驱动 .ko 与 device_monitor 放在同一目录

cd "$(dirname "$0")" || exit 1

# 1. 加载驱动（已加载的跳过）
for m in led key ap3216c icm20608 gt911; do
	if ! grep -q "^${m}_drv " /proc/modules; then
		insmod "./${m}_drv.ko" || echo "insmod ${m}_drv.ko failed"
	fi
done
sleep 1

# 2. 按 input 设备名查找节点：eventX 编号随驱动加载顺序变化，不能写死
find_event() {
	for d in /sys/class/input/event*; do
		if [ "$(cat "$d/device/name" 2>/dev/null)" = "$1" ]; then
			echo "/dev/input/${d##*/}"
			return 0
		fi
	done
	return 1
}

TOUCH_DEV=$(find_event gt911) || echo "touch device (gt911) not found"
KEY_DEV=$(find_event keyinput) || echo "key device (keyinput) not found"
export DEVICE_MONITOR_KEY_DEV="$KEY_DEV"
echo "touch: ${TOUCH_DEV:-none}  key: ${KEY_DEV:-none}"

# 3. Qt 运行环境
export QT_ROOT=/usr/lib/arm-qt
export QT_QPA_PLATFORM_PLUGIN_PATH=$QT_ROOT/plugins
export QT_PLUGIN_PATH=$QT_ROOT/plugins
export QT_QPA_FONTDIR=/usr/share/fonts
export QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0
export LD_LIBRARY_PATH=$QT_ROOT/lib:$LD_LIBRARY_PATH
unset QT_QPA_GENERIC_PLUGINS
# Qt 需要一个仅本用户可访问的运行时目录，busybox 系统默认不设置
export XDG_RUNTIME_DIR=/tmp/runtime-root
mkdir -p "$XDG_RUNTIME_DIR" && chmod 0700 "$XDG_RUNTIME_DIR"

if [ "$TOUCH" = "evdev" ]; then
	# evdevtouch 自动扫描触摸设备，这里显式指定节点
	unset QT_QPA_FB_TSLIB
	export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS="$TOUCH_DEV"
else
	# linuxfb 在 QT_QPA_FB_TSLIB=1 时自己创建 tslib 输入，不要再设置
	# QT_QPA_GENERIC_PLUGINS=tslib，否则会多出一套重复的触摸输入
	export TSLIB_ROOT=/usr/lib/arm-tslib
	export TSLIB_CONSOLEDEVICE=none
	export TSLIB_FBDEVICE=/dev/fb0
	export TSLIB_TSDEVICE="$TOUCH_DEV"
	export TSLIB_CONFFILE=$TSLIB_ROOT/etc/ts.conf
	export TSLIB_PLUGINDIR=$TSLIB_ROOT/lib/ts
	unset TSLIB_CALIBFILE # 电容屏不需要校准文件
	export LD_LIBRARY_PATH=$TSLIB_ROOT/lib:$LD_LIBRARY_PATH
	export QT_QPA_FB_TSLIB=1
fi

# 4. 取消屏幕消隐、关掉控制台光标闪烁
echo 0 > /sys/class/graphics/fb0/blank
echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null

exec ./device_monitor "$@"
