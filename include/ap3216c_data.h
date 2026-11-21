#ifndef __AP3216C_DATA_H
#define __AP3216C_DATA_H

/*
 * AP3216C 各通道有效标志（应用内部使用）。
 * 驱动已改为 IIO（drivers/ap3216c.c）：IR 溢出时 in_intensity_ir_raw / in_proximity_raw
 * 读返回 EAGAIN，应用据此清除对应标志。原字符设备的 struct ap3216c_data 已不再使用。
 */
#define AP3216C_IR_VALID  0x01
#define AP3216C_ALS_VALID 0x02
#define AP3216C_PS_VALID  0x04

#endif
