/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * 驱动和用户态程序共用的数据格式：read(/dev/icm20608) 每次返回一个
 * struct icm20608_sample。数值是芯片原始计数，已转换成 CPU 字节序。
 *
 * 量程（驱动初始化时固定）：
 *   加速度 +-2 g        -> 16384 LSB/g
 *   陀螺仪 +-2000 deg/s -> 16.4  LSB/(deg/s)
 *   温度               -> degC = temp / 326.8 + 25
 */
#ifndef _ICM20608_UAPI_H
#define _ICM20608_UAPI_H

#include <linux/types.h>

#define ICM20608_ACCEL_LSB_PER_G	16384
#define ICM20608_GYRO_LSB_PER_DPS_X10	164	/* 16.4 LSB/(deg/s) 乘 10 */

struct icm20608_sample {
	__s16 accel[3];		/* x, y, z */
	__s16 temp;
	__s16 gyro[3];		/* x, y, z */
};

#endif /* _ICM20608_UAPI_H */
