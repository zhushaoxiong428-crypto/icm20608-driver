#!/bin/sh
# 读 ICM-20608 的 IIO sysfs 接口，换算成物理量：value = (raw + offset) * scale
# 用法：./iio_read.sh [次数] [间隔秒]，默认 5 次、间隔 0.5 秒（板子上的 busybox sh 运行）

N=${1:-5}
INTERVAL=${2:-0.5}

DEV=""
for d in /sys/bus/iio/devices/iio:device*; do
	case "$(cat "$d/name")" in
	icm20608*) DEV=$d ;;
	esac
done
[ -n "$DEV" ] || { echo "no icm20608 IIO device"; exit 1; }
echo "device: $DEV ($(cat "$DEV/name"))"

as=$(cat "$DEV/in_accel_scale")
gs=$(cat "$DEV/in_anglvel_scale")
ts=$(cat "$DEV/in_temp_scale")
to=$(cat "$DEV/in_temp_offset")
echo "accel scale $as m/s^2, anglvel scale $gs rad/s, temp scale $ts mC, offset $to"

printf "%9s %9s %9s | %9s %9s %9s | %6s\n" "ax[m/s2]" "ay" "az" "gx[rad/s]" "gy" "gz" "T[C]"
i=0
while [ $i -lt "$N" ]; do
	ax=$(cat "$DEV/in_accel_x_raw");   ay=$(cat "$DEV/in_accel_y_raw");   az=$(cat "$DEV/in_accel_z_raw")
	gx=$(cat "$DEV/in_anglvel_x_raw"); gy=$(cat "$DEV/in_anglvel_y_raw"); gz=$(cat "$DEV/in_anglvel_z_raw")
	t=$(cat "$DEV/in_temp_raw")
	echo "$ax $ay $az $gx $gy $gz $t" | awk -v as="$as" -v gs="$gs" -v ts="$ts" -v to="$to" \
		'{ printf "%9.3f %9.3f %9.3f | %9.4f %9.4f %9.4f | %6.2f\n",
		   $1*as, $2*as, $3*as, $4*gs, $5*gs, $6*gs, ($7+to)*ts/1000 }'
	i=$((i + 1))
	sleep "$INTERVAL"
done
