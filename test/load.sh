#!/bin/sh
# v4 负载：把单核 i.MX6ULL 压满
#   start：2 个纯 CPU 死循环 + 1 个系统调用密集的 dd
#   heavy：8 个死循环 + dd + 通过 NFS 循环读文件（网卡中断、协议栈也忙起来）
# 用法：./load.sh start | heavy | stop
PIDS=/tmp/load.pids
case "$1" in
start|heavy)
	rm -f $PIDS
	N=2
	[ "$1" = heavy ] && N=8
	for i in $(seq $N); do
		( while :; do :; done ) &
		echo $! >> $PIDS
	done
	( while :; do dd if=/dev/zero of=/dev/null bs=512 count=200000 2>/dev/null; done ) &
	echo $! >> $PIDS
	if [ "$1" = heavy ]; then
		( while :; do cat /bin/busybox /lib/*.so* > /dev/null 2>&1; done ) &
		echo $! >> $PIDS
	fi
	echo "load $1: $(cat $PIDS | tr '\n' ' ')"
	;;
stop)
	[ -f $PIDS ] && kill $(cat $PIDS) 2>/dev/null
	killall dd cat 2>/dev/null
	rm -f $PIDS
	echo "load stopped"
	;;
*)
	echo "usage: $0 start|heavy|stop"
	;;
esac
