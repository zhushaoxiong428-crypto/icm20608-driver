# ICM-20608 六轴 IMU Linux 驱动（i.MX6ULL）

在正点原子 ALPHA 开发板（i.MX6ULL，Linux 4.1.15）上，从零为板载 SPI 六轴 IMU ICM-20608-D 编写的内核驱动：设备树 → SPI 驱动 → regmap → IIO 子系统 → 数据就绪中断 + triggered buffer → 芯片硬件 FIFO + 实时读取线程，并与用户态 spidev 轮询、与自身的中断版本做了负载下的实测对比。

## 结果

1 kHz 采样、每组 10 s，单核 CPU 被 8 个死循环 + dd + NFS I/O 压满（详见 [doc/v4_results.md](doc/v4_results.md)）：

| 方式 | 实际频率 | 采样间隔 sd | 间隔最大 | 丢样 |
|---|---|---|---|---|
| **本驱动（中断 + IIO 缓冲区）** | 1000.9 Hz | **5.3 µs** | 1.06 ms | **0** |
| 用户态 spidev 轮询，SCHED_FIFO | 1000.0 Hz | 58.2 µs | 2.26 ms | 1.2% |
| 用户态 spidev 轮询，普通调度 | 702 Hz | 2477 µs | 20.1 ms | 61% |

v5 改读芯片硬件 FIFO 后，与 v3 的中断方式对比（1 kHz，详见 [doc/v5_results.md](doc/v5_results.md)）：

| 方式 | 中断/s | 上下文切换/s | 高优先级实时任务每秒占满 CPU 20 ms 时丢样 |
|---|---|---|---|
| v3：每样本数据就绪中断 | 5013 | 8026 | 198（2.0%） |
| **v5：硬件 FIFO，每批 10 个** | **1553** | **1286** | **0** |
| v5：硬件 FIFO，每批 24 个 | 766 | 512 | — |

## 版本

| 版本 | 内容 | 遇到的问题 |
|---|---|---|
| v0 | 设备树启用 ECSPI3、添加 IMU 节点；`spi_driver` probe 读 WHO_AM_I | IMU 的 SPI 引脚与 UART2、CAN2 复用同一组 pad，需关闭两者；读到 0xAE 而非手册的 0xAF，板载实为 ICM-20608-D，按主线 compatible 改为多型号匹配 |
| v1 | 芯片初始化；字符设备 `/dev/icm20608`（tag `v1`） | 写后回读校验发现复位后芯片处于睡眠，USER_CTRL 写不进去，改为先唤醒再配置 |
| v2 | regmap-spi + IIO 子系统，7 个通道，SI 单位的 scale/offset | 4.1 默认未开 IIO；`IIO_TRIGGERED_BUFFER` 为隐藏选项，需借 `INV_MPU6050_IIO` select 出来 |
| v3 | 数据就绪中断（GPIO1_IO10 上升沿）：硬中断记时间戳，线程化下半部 SPI 读数据推入 IIO 缓冲区；采样率可调 | 修正一处对 IIO 触发器的重复 `put`（核心在释放 iio_dev 时已经 put） |
| v4 | 与用户态 spidev 轮询的负载对比；与主线 `inv_mpu6050` 的差异分析 | 见 [doc/v4_results.md](doc/v4_results.md)、[doc/v4_mainline_diff.md](doc/v4_mainline_diff.md) |
| v5 | 硬件 FIFO（默认，`fifo=0` 切回 v3）：芯片没有 FIFO 水位中断，由 SCHED_FIFO 内核线程按 `buffer/watermark` 定时批量读；溢出检测与计数（`fifo_overflows`）；批量样本的时间戳按芯片实际周期推算，误差不超过一个周期；`buffer/hwfifo_*` 属性 | 时间戳窗口只取读完计数后的时刻，被抢占时整批被拉长、统计出假丢样，改为读计数前后各取一次；重负载下偶发一次溢出，推断为 SPI 消息泵（普通优先级）造成的优先级反转，驱动把它改为实时（`spi_rt`），对照实验未能复现。见 [doc/v5_results.md](doc/v5_results.md) |

## 目录

```
driver/   icm20608.c、Makefile（外部模块）
dts/      对正点原子教程内核设备树的补丁（0001 正式，0002 v4 测试用 spidev 版本）
kernel/   内核配置片段（IIO、spidev）
test/     iio_read.sh（sysfs 单次读）、iio_buffer.c（缓冲区连续采集 + 间隔、中断、溢出统计）、
          spidev_poll.c（用户态轮询对照组）、load.sh（负载）、
          rt_hog.c（以高于驱动线程的实时优先级周期性占满 CPU）、v5_bench.sh（v5 对比实验）
doc/      实测报告、主线对照、原始数据
```

## 构建与运行

```bash
# 内核：正点原子教程源码 linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek（V2.4 底板）
#   打 dts/0001 补丁，按 kernel/iio.config 打开 IIO，编译 zImage + dtb
# 驱动
cd driver && make KERNELDIR=<内核源码目录>
# 测试程序
cd test && make
# 板上
insmod icm20608.ko           # v5 硬件 FIFO；fifo=0 为 v3 数据就绪中断；spi_rt=0 不改 SPI 消息泵优先级
./iio_read.sh 5 0.5          # 单次读，换算成 m/s^2、rad/s、degC
./iio_buffer 1000 10 3 10    # 1 kHz 连续采集 10 s，每批 10 个（watermark），统计间隔、丢样、中断、溢出
cat /sys/bus/iio/devices/iio:device1/fifo_overflows
# v3 卸载前要先解除默认触发器（4.1 的 iio_trigger_get 会增加模块引用计数）：
echo "" > /sys/bus/iio/devices/iio:device1/trigger/current_trigger; rmmod icm20608
```

工具链：Linaro GCC 4.9.4（arm-linux-gnueabihf）。开发方式：TFTP 加载内核与设备树，NFS 挂载根文件系统。
