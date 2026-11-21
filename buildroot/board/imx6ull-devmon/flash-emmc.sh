#!/bin/sh
# 在板子上运行：把 Buildroot 系统写进 eMMC（板子必须从 NFS 启动，eMMC 分区不能处于挂载状态）
#   p1 (FAT)  zImage 和出厂 U-Boot 选用的 800x480 设备树换成本项目的；原文件改名 *.factory 保留
#   p2 (ext4) 写入 rootfs.ext4（256 MB），首次启动由 S01growfs 在线扩展到整个分区
# eMMC 开头 10 MB（U-Boot、环境变量、分区表）和 boot0/boot1 分区都不动，出厂 U-Boot 照常启动、照常能网络启动
# 用法：flash-emmc.sh <目录>   目录里要有 zImage、imx6ull-device-management.dtb、rootfs.ext4
set -e

SRC=${1:?usage: flash-emmc.sh <dir with zImage, imx6ull-device-management.dtb, rootfs.ext4>}
DEV=/dev/mmcblk1
FDT=imx6ull-14x14-emmc-4.3-800x480-c.dtb # 出厂 U-Boot 按屏幕 ID 选的设备树文件名（ATK-4384）
MNT=/tmp/emmc-p1

for f in zImage imx6ull-device-management.dtb rootfs.ext4; do
	[ -f "$SRC/$f" ] || { echo "missing $SRC/$f"; exit 1; }
done
grep -q 'root=/dev/nfs' /proc/cmdline || { echo "refusing: boot from NFS first"; exit 1; }
if grep -q "^$DEV" /proc/mounts; then
	echo "refusing: an eMMC partition is mounted"
	exit 1
fi

echo "[1/3] p1: kernel and device tree"
mkdir -p "$MNT"
mount -t vfat "${DEV}p1" "$MNT"
# 只在第一次烧录时保存出厂文件，重复烧录不会把出厂文件覆盖成本项目的
[ -f "$MNT/zImage.factory" ] || cp "$MNT/zImage" "$MNT/zImage.factory"
[ -f "$MNT/$FDT.factory" ] || cp "$MNT/$FDT" "$MNT/$FDT.factory"
cp "$SRC/zImage" "$MNT/zImage"
cp "$SRC/imx6ull-device-management.dtb" "$MNT/$FDT"
sync
umount "$MNT"

echo "[2/3] p2: root filesystem"
dd if="$SRC/rootfs.ext4" of="${DEV}p2" bs=1M
sync

echo "[3/3] verify (read back from eMMC, not the page cache)"
echo 3 > /proc/sys/vm/drop_caches
SIZE=$(wc -c < "$SRC/rootfs.ext4")
[ "$(md5sum < "$SRC/rootfs.ext4")" = "$(head -c "$SIZE" "${DEV}p2" | md5sum)" ] ||
	{ echo "verify FAILED: p2"; exit 1; }
mount -t vfat -o ro "${DEV}p1" "$MNT"
ok=1
cmp -s "$SRC/zImage" "$MNT/zImage" || ok=0
cmp -s "$SRC/imx6ull-device-management.dtb" "$MNT/$FDT" || ok=0
ls -l "$MNT"
umount "$MNT"
[ $ok = 1 ] || { echo "verify FAILED: p1"; exit 1; }

echo "done: reset the board without interrupting U-Boot to boot from eMMC"
