#!/bin/sh
# 开发板上启动：加载驱动 → 启动采集守护进程 devmond → 设置 tslib/Qt 环境变量 → 运行界面
# 用法：./run_device_monitor.sh                触摸走 tslib（正点原子手册方式）
#       TOUCH=evdev ./run_device_monitor.sh    触摸走 Qt 自带 evdevtouch（支持多点，不需要 tslib）
#       ./run_device_monitor.sh --daemon-only  只启动 devmond（无屏幕运行、或界面稍后再开）
# devmond 已在运行时不会重复启动（pid 文件锁）；界面退出后采集与录制继续。
# 停止 devmond：kill $(cat /tmp/devmond.pid)（只发给监管进程，它会停工作进程并关闭看门狗）
# 环境变量：TZ（默认 CST-8 北京时间）、NTP_SERVER（默认 192.168.10.1，即 TFTP/NFS 服务器）、
#           DEVMOND_TCP_PORT（默认 9100，0 关闭远程访问）、
#           WATCHDOG（硬件看门狗超时秒数，默认 15；开发调试时设 0，避免停掉 devmond 后整板复位）、
#           MQTT_BROKER（默认 192.168.10.1，设为空字符串关闭 MQTT）、MQTT_TOPIC（默认 devmon/主机名）
# 约定路径（与正点原子《移植Qt5.12.9》一致）：Qt 在 /usr/lib/arm-qt，tslib 在 /usr/lib/arm-tslib，
# 字体在 /usr/share/fonts；本脚本、驱动 .ko 与 device_monitor 放在同一目录

cd "$(dirname "$0")" || exit 1

# 本地时区（POSIX 格式，busybox 没有时区数据库）：CSV、日志、界面按北京时间显示；
# 内核时钟、NTP 和 RTC 一律用 UTC，只有显示和记录换算成本地时间
export TZ=${TZ:-CST-8}

# 1. 加载驱动（已加载的跳过）。ap3216c.ko、icm20608.ko 为 IIO 驱动（drivers/、icm20608-driver/）
for m in led_drv key_drv ap3216c icm20608 gt911_drv; do
	if ! grep -q "^${m} " /proc/modules; then
		insmod "./${m}.ko" || echo "insmod ${m}.ko failed"
	fi
done
sleep 1

# 2. 校时：busybox ntpd 向 NTP 服务器同步，每次校准后由 ntp_event.sh 写入 SNVS RTC，
#    断网重启时内核从 RTC 恢复时间（需要底板装纽扣电池才能断电保持）
NTP_SERVER=${NTP_SERVER:-192.168.10.1}
if ! pidof ntpd > /dev/null; then
	ntpd -p "$NTP_SERVER" -S "$PWD/ntp_event.sh" && echo "ntpd started, server $NTP_SERVER"
fi

# 3. 采集守护进程：传感器、LED、按键、自动亮灯、参数和 CSV 录制都在这里；
#    -t 同一协议再监听 TCP 端口供远程访问（局域网使用，无认证）
#    监管进程负责重启崩溃/卡死的工作进程，并用 -w 喂硬件看门狗
#    内核 panic 后 10 秒自动重启（默认 0 表示停在 panic 现场不动）
echo 10 > /proc/sys/kernel/panic
DEVMOND_TCP_PORT=${DEVMOND_TCP_PORT:-9100}
WATCHDOG=${WATCHDOG:-15}
# MQTT：所有推送转发到 broker，并从 <主题>/cmd 接收命令；broker 不在时 devmond 自己退避重试
MQTT_BROKER=${MQTT_BROKER-192.168.10.1}
if pidof devmond > /dev/null; then
	echo "devmond already running (pid $(cat /tmp/devmond.pid 2>/dev/null))"
else
	./devmond -d -l /tmp/devmond.log -t "$DEVMOND_TCP_PORT" -w "$WATCHDOG" \
		${MQTT_BROKER:+-m "$MQTT_BROKER"} ${MQTT_TOPIC:+-M "$MQTT_TOPIC"} \
		&& echo "devmond started, log /tmp/devmond.log, tcp port $DEVMOND_TCP_PORT, watchdog $WATCHDOG s, mqtt ${MQTT_BROKER:-off}"
fi
[ "$1" = "--daemon-only" ] && exit 0

# 4. 按 input 设备名查找触摸屏节点：eventX 编号随驱动加载顺序变化，不能写死
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
echo "touch: ${TOUCH_DEV:-none}"

# 5. Qt 运行环境
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

# 6. 取消屏幕消隐、关掉控制台光标闪烁
echo 0 > /sys/class/graphics/fb0/blank
printf '\033[9;0]' > /dev/tty1 2>/dev/null # 关闭控制台空闲黑屏（默认 10 分钟后黑屏）
echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null

# 7. 界面崩溃时自动重启；点 Exit 正常退出（返回 0）时不再拉起。
#    循环所在 shell 的 pid 记在 /tmp/device_monitor_ui.pid，S90device-monitor stop 先停它，界面就不会被重新拉起
echo $$ > /tmp/device_monitor_ui.pid
while true; do
	./device_monitor "$@"
	rc=$?
	[ $rc -eq 0 ] && break
	echo "device_monitor exited with status $rc, restarting in 2 s"
	sleep 2
done
rm -f /tmp/device_monitor_ui.pid
