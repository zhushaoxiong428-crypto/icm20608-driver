#!/bin/sh
# 在板子上运行：把整盘备份写回 eMMC，恢复出厂系统（板子必须从 NFS 启动）
# 备份是烧录前在板子上做的：dd if=/dev/mmcblk1 of=emmc-factory-mmcblk1.img（boot0/boot1 同理）
# 用法：restore-emmc.sh <备份目录> [--boot]
#   默认只写用户区（分区表、U-Boot、两个分区，7.3 GB，经 NFS 约 13 分钟）
#   --boot 同时写回 boot0/boot1（本项目烧录不会改动它们，一般不需要）
set -e

DIR=${1:?usage: restore-emmc.sh <backup dir> [--boot]}
DEV=/dev/mmcblk1
IMG=$DIR/emmc-factory-mmcblk1.img

[ -f "$IMG" ] || { echo "missing $IMG"; exit 1; }
grep -q 'root=/dev/nfs' /proc/cmdline || { echo "refusing: boot from NFS first"; exit 1; }
if grep -q "^$DEV" /proc/mounts; then
	echo "refusing: an eMMC partition is mounted"
	exit 1
fi
# 镜像必须正好是整个用户区大小（/sys/block/.../size 以 512 字节扇区计）
[ "$(wc -c < "$IMG")" = "$(($(cat /sys/block/mmcblk1/size) * 512))" ] ||
	{ echo "refusing: image size does not match $DEV"; exit 1; }

echo "writing $IMG -> $DEV"
dd if="$IMG" of="$DEV" bs=1M
sync

if [ "$2" = "--boot" ]; then
	for p in boot0 boot1; do
		echo 0 > /sys/block/mmcblk1$p/force_ro
		dd if="$DIR/emmc-factory-mmcblk1$p.img" of=/dev/mmcblk1$p bs=1M
		sync
		echo 1 > /sys/block/mmcblk1$p/force_ro
	done
fi

echo "done: reset the board to boot the factory system"
