#!/bin/sh
# Buildroot post-build：把仓库里已编译好的设备监测系统装进根文件系统（二进制不进 git）
#   $1 = TARGET_DIR（Buildroot 传入）
# 环境变量可覆盖：
#   QT_SDK     ARM Qt 5.12.9 安装目录（交叉编译时的 -prefix，含 lib/ plugins/）
#   TSLIB_DIR  ARM tslib 安装目录
#   FONT_DIR   字体来源（DejaVu Sans / Sans Mono）
# 事先在仓库根目录执行 make（驱动、devmond）和 Qt 交叉编译（build/qt-device-monitor）。
set -e

TARGET=$1
BOARD_DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$BOARD_DIR/../../.." && pwd)
QT_SDK=${QT_SDK:-$HOME/alpha/src/qt-everywhere-src-5.12.9/arm-qt}
TSLIB_DIR=${TSLIB_DIR:-$HOME/alpha/src/tslib-1.21/arm-tslib}
FONT_DIR=${FONT_DIR:-/usr/share/fonts/truetype/dejavu}
APP=$TARGET/opt/device-monitor
STRIP=$HOST_DIR/bin/arm-linux-gnueabihf-strip

BINARIES="build/devmond build/devmonctl build/qt-device-monitor/device_monitor
	build/iio_buffer build/iio_event_monitor build/ap3216c_i2c_bench build/touch_test"
MODULES="drivers/led_drv.ko drivers/key_drv.ko drivers/ap3216c.ko drivers/gt911_drv.ko
	icm20608-driver/driver/icm20608.ko"
SCRIPTS="qt/device_monitor/run_device_monitor.sh qt/device_monitor/ntp_event.sh"

# 缺文件直接失败，避免生成一个能启动却没有应用的根文件系统
for f in $BINARIES $MODULES $SCRIPTS qt/device_monitor/S90device-monitor; do
	[ -f "$REPO/$f" ] || { echo "post-build: missing $REPO/$f (run make / build Qt first)"; exit 1; }
done
for d in "$QT_SDK/lib" "$QT_SDK/plugins/platforms" "$TSLIB_DIR/lib"; do
	[ -d "$d" ] || { echo "post-build: missing $d (set QT_SDK / TSLIB_DIR)"; exit 1; }
done

# 应用：程序、驱动、脚本在同一目录（启动脚本按自身所在目录工作，配置和录制也放这里）
mkdir -p "$APP"
for f in $BINARIES; do
	install -m 0755 "$REPO/$f" "$APP/"
	"$STRIP" "$APP/$(basename "$f")"
done
for f in $MODULES; do
	install -m 0644 "$REPO/$f" "$APP/"
done
for f in $SCRIPTS; do
	install -m 0755 "$REPO/$f" "$APP/"
done
install -m 0755 "$REPO/qt/device_monitor/S90device-monitor" "$TARGET/etc/init.d/S90device-monitor"
touch "$APP/autostart" # 默认开机启动；在板子上 rm /opt/device-monitor/autostart 关闭

# 驱动由启动脚本从 $APP insmod，不装进 /lib/modules；但 S10mdev 冷插拔时会按 modalias 调 modprobe，
# 没有这个目录会打印 "can't change directory to '/lib/modules'"，放一个空的 modules.dep
mkdir -p "$TARGET/lib/modules/${KERNEL_RELEASE:-4.1.15}"
: > "$TARGET/lib/modules/${KERNEL_RELEASE:-4.1.15}/modules.dep"

# Qt 运行库：只装界面用到的模块（Widgets 界面 + Network 套接字；linuxfb 插件依赖 DBus）和
# 平台、输入、图片格式插件。整个 SDK 的库和插件有 28 MB，这样约 14 MB；缺什么由下面的依赖检查报出来
QT_LIBS="Core Gui Widgets Network DBus"
QT_PLUGINS="platforms generic imageformats"
rm -rf "$TARGET/usr/lib/arm-qt"
mkdir -p "$TARGET/usr/lib/arm-qt/lib" "$TARGET/usr/lib/arm-qt/plugins"
for m in $QT_LIBS; do
	cp -a "$QT_SDK"/lib/libQt5$m.so* "$TARGET/usr/lib/arm-qt/lib/"
done
for p in $QT_PLUGINS; do
	cp -a "$QT_SDK/plugins/$p" "$TARGET/usr/lib/arm-qt/plugins/"
done

# tslib：库、插件、配置，以及 ts_test 等调试工具
rm -rf "$TARGET/usr/lib/arm-tslib"
cp -a "$TSLIB_DIR" "$TARGET/usr/lib/arm-tslib"
rm -rf "$TARGET/usr/lib/arm-tslib/include" "$TARGET/usr/lib/arm-tslib/share"

# 字体：界面把 Consolas 映射到 DejaVu Sans Mono
mkdir -p "$TARGET/usr/share/fonts"
for f in DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf; do
	install -m 0644 "$FONT_DIR/$f" "$TARGET/usr/share/fonts/"
done

# 依赖检查：应用、Qt 库和插件、tslib 需要的每个共享库都必须在根文件系统里，
# 缺库在构建时失败，而不是到板子上才报 "error while loading shared libraries"
READELF=$HOST_DIR/bin/arm-linux-gnueabihf-readelf
LIBDIRS="$TARGET/lib $TARGET/usr/lib $TARGET/usr/lib/arm-qt/lib $TARGET/usr/lib/arm-tslib/lib"
missing=0
for elf in $(find "$APP" "$TARGET/usr/lib/arm-qt" "$TARGET/usr/lib/arm-tslib" -type f \( -perm -u+x -o -name '*.so*' \)); do
	LC_ALL=C "$READELF" -h "$elf" > /dev/null 2>&1 || continue # 跳过脚本
	for lib in $(LC_ALL=C "$READELF" -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
		found=0
		for d in $LIBDIRS; do
			[ -e "$d/$lib" ] && { found=1; break; }
		done
		[ $found = 1 ] || { echo "post-build: ${elf#$TARGET} needs $lib"; missing=1; }
	done
done
[ $missing = 0 ] || exit 1

echo "post-build: device monitor installed to /opt/device-monitor ($(du -sh "$TARGET" | cut -f1) rootfs)"
