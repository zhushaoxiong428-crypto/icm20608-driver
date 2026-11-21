#!/bin/sh
# busybox ntpd -S 回调：$1 为 step（时间跳变）、stratum（层级变化）、periodic（约 11 分钟一次）、unsync。
# 已同步时把系统时间写入 SNVS RTC（按 UTC 保存），下次启动内核从 RTC 恢复，不会回到 1970 或旧时间
[ "$1" = "unsync" ] && exit 0
hwclock -w -u
