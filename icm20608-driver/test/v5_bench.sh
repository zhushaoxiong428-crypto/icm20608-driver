#!/bin/sh
# v5 对比实验：同一内核、同一测试程序，v3（fifo=0，每样本中断）对比 v5（硬件 FIFO）
#   A 空闲 1 kHz：v3、v5 watermark 1 / 10 / 24
#   B 重负载 1 kHz（load.sh heavy）：v3、v5 wm 10
#   C 实时任务每秒占满 CPU 5 / 20 / 50 ms（rt_hog，SCHED_FIFO 60 > 驱动线程 50）：v3、v5 wm 10
#   D 100 Hz（devmond 的实际用法）：v3、v5 wm 10
#   E 重负载 60 s：v3、v5 各在 spi_rt=0（SPI 消息泵保持普通优先级）和 spi_rt=1 下跑一次
# A–D 用驱动默认参数（spi_rt=1）
# 用法：在 iio_buffer、rt_hog、load.sh、icm20608.ko 所在目录运行 ./v5_bench.sh <输出文件>
# 运行前先停掉占用 IMU 的程序（S90device-monitor stop）
OUT=${1:-v5_bench.txt}
cd "$(dirname "$0")"

load_driver() {
	# v3 把自己的数据就绪触发器设为默认触发器，4.1 的 iio_trigger_get() 会同时增加模块引用计数，
	# 不先解除 current_trigger 就 rmmod 不掉（v5 没有触发器）
	for d in /sys/bus/iio/devices/iio:device*; do
		case "$(cat "$d/name" 2>/dev/null)" in
		icm20608*) [ -f "$d/trigger/current_trigger" ] && echo "" > "$d/trigger/current_trigger" ;;
		esac
	done
	rmmod icm20608 2>/dev/null
	insmod ./icm20608.ko fifo=$1 spi_rt=${2:-1} || exit 1
	sleep 1
}

# run <标签> <fifo=0|1> <采样率> <watermark> [秒数] [spi_rt]
run() {
	load_driver "$2" "${6:-1}"
	echo "=== $1 (fifo=$2, $3 Hz, watermark $4, spi_rt=${6:-1})" >> "$OUT"
	./iio_buffer "$3" "${5:-10}" 0 "$4" >> "$OUT" 2>&1
	echo >> "$OUT"
}

: > "$OUT"
echo "v5 bench $(date)" >> "$OUT"

run A1_v3 0 1000 1
run A2_v5_wm1 1 1000 1
run A3_v5_wm10 1 1000 10
run A4_v5_wm24 1 1000 24

./load.sh heavy > /dev/null
sleep 2
run B1_v3_heavy 0 1000 1
run B2_v5_wm10_heavy 1 1000 10
./load.sh stop > /dev/null
sleep 2

for ms in 5 20 50; do
	for m in 0 1; do
		wm=1
		[ $m = 1 ] && wm=10
		load_driver $m
		./rt_hog $ms 1000 12 > /dev/null &
		hog=$!
		echo "=== C_hog${ms}ms_$( [ $m = 0 ] && echo v3 || echo v5_wm10 ) (fifo=$m, 1000 Hz, watermark $wm, rt_hog $ms ms/s)" >> "$OUT"
		./iio_buffer 1000 10 0 $wm >> "$OUT" 2>&1
		echo >> "$OUT"
		wait $hog
	done
done

run D1_v3_100hz 0 100 1
run D2_v5_wm10_100hz 1 100 10

./load.sh heavy > /dev/null
sleep 2
run E1_v3_heavy60_pumpnormal 0 1000 1 60 0
run E2_v3_heavy60_pumprt 0 1000 1 60 1
run E3_v5_heavy60_pumpnormal 1 1000 10 60 0
run E4_v5_heavy60_pumprt 1 1000 10 60 1
./load.sh stop > /dev/null

load_driver 1
echo DONE >> "$OUT"
