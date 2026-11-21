// SPDX-License-Identifier: GPL-2.0
/*
 * ICM-20608-G / ICM-20608-D 六轴 IMU 的 SPI + IIO 驱动
 *
 * 寄存器访问用 regmap，数据通过 IIO 子系统导出：
 *   /sys/bus/iio/devices/iio:deviceN/in_accel_{x,y,z}_raw     + in_accel_scale
 *   /sys/bus/iio/devices/iio:deviceN/in_anglvel_{x,y,z}_raw   + in_anglvel_scale
 *   /sys/bus/iio/devices/iio:deviceN/in_temp_{raw,offset,scale}
 * 物理量 = (raw + offset) * scale，单位：m/s^2、rad/s、毫摄氏度
 *
 * v3：芯片每采完一个样本就在 INT 引脚发 50 us 脉冲（数据就绪中断）。
 *   上半部（硬中断）：iio_trigger_generic_data_rdy_poll -> 触发 IIO 触发器
 *                     iio_pollfunc_store_time          -> 记录时间戳
 *   下半部（内核线程）：icm20608_trigger_handler        -> SPI 读 14 字节，
 *                     连同时间戳推进 IIO 缓冲区，用户读 /dev/iio:deviceN
 *   下半部必须在下一个样本到来前（1 kHz 时 1 ms 内）读走数据寄存器，否则样本被覆盖且无法察觉。
 *
 * v5（默认）：芯片硬件 FIFO。芯片把每个样本的 14 字节依次写进内部 512 字节 FIFO（36 个样本），
 *   不再每个样本中断一次：一个实时内核线程（SCHED_FIFO 50，与中断线程同级）用高精度定时器
 *   每 watermark 个采样周期醒来，读 FIFO_COUNT，再一次 SPI 连续读出全部完整样本推进缓冲区。
 *   ICM-20608 没有 FIFO 水位中断（ICM-20602 才有），所以用定时器代替。
 *   - 抗延迟：线程迟到 (36 - watermark) 个周期以内都不丢数据
 *   - 溢出可检测：FIFO 放不下下一个样本时复位，计数在 fifo_overflows
 *   - 时间戳：一批样本没有各自的中断时刻，按芯片实际输出周期推算（icm20608_fifo_step）
 *   insmod icm20608.ko fifo=0 切回 v3，用于对比。
 */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/iio/sysfs.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/math64.h>

static bool fifo = true;
module_param(fifo, bool, 0444);
MODULE_PARM_DESC(fifo, "use the hardware FIFO (v5, default); 0 = one data-ready interrupt per sample (v3)");

/*
 * SPI 控制器消息泵线程用实时优先级。4.1 的 SPI 核心在每次传输结束后，只在消息泵线程里做
 * "队列空了 -> 关硬件" 的收尾；收尾期间来的 spi_sync() 会被推迟给这个线程执行。它默认是
 * 普通优先级，重负载下收尾到一半被抢占，实时的读数线程就要等它几十 ms（优先级反转）。
 * 新内核里 SPI 设备可以申请实时消息泵（主线 spi_set_thread_rt），4.1 没有，这里照做。
 */
static bool spi_rt = true;
module_param(spi_rt, bool, 0444);
MODULE_PARM_DESC(spi_rt, "run the SPI controller message pump at SCHED_FIFO 50 (default); 0 = leave it as is");

/* 寄存器地址（ICM-20608 Register Map） */
#define ICM20608_REG_SMPLRT_DIV		0x19
#define ICM20608_REG_CONFIG		0x1A
#define ICM20608_REG_GYRO_CONFIG	0x1B
#define ICM20608_REG_ACCEL_CONFIG	0x1C
#define ICM20608_REG_ACCEL_CONFIG2	0x1D
#define ICM20608_REG_FIFO_EN		0x23
#define ICM20608_REG_INT_PIN_CFG	0x37
#define ICM20608_REG_INT_ENABLE		0x38
#define ICM20608_REG_ACCEL_XOUT_H	0x3B
#define ICM20608_REG_ACCEL_YOUT_H	0x3D
#define ICM20608_REG_ACCEL_ZOUT_H	0x3F
#define ICM20608_REG_TEMP_OUT_H		0x41
#define ICM20608_REG_GYRO_XOUT_H	0x43
#define ICM20608_REG_GYRO_YOUT_H	0x45
#define ICM20608_REG_GYRO_ZOUT_H	0x47
#define ICM20608_REG_USER_CTRL		0x6A
#define ICM20608_REG_PWR_MGMT_1		0x6B
#define ICM20608_REG_PWR_MGMT_2		0x6C
#define ICM20608_REG_FIFO_COUNTH	0x72	/* 高字节在前，0x73 为低字节 */
#define ICM20608_REG_FIFO_R_W		0x74	/* 连续读这个地址就是依次读 FIFO */
#define ICM20608_REG_WHO_AM_I		0x75

/* 位定义 */
#define ICM20608_PWR1_DEVICE_RESET	0x80	/* 复位，完成后自动清零 */
#define ICM20608_PWR1_SLEEP		0x40	/* 进入睡眠 */
#define ICM20608_PWR1_CLKSEL_AUTO	0x01	/* 自动选择最佳时钟，同时 SLEEP=0 */
#define ICM20608_USER_I2C_IF_DIS	0x10	/* 关闭 I2C 接口，只用 SPI */
#define ICM20608_GYRO_FS_2000DPS	(3 << 3)	/* 16.4 LSB/(deg/s) */
#define ICM20608_ACCEL_FS_2G		(0 << 3)	/* 16384 LSB/g */
#define ICM20608_DLPF_CFG		3	/* 陀螺仪带宽约 41 Hz */
#define ICM20608_A_DLPF_CFG		3	/* 加速度计带宽约 44.8 Hz */
#define ICM20608_INT_PIN_CFG_DEFAULT	0x00	/* 高有效、推挽、50 us 脉冲 */
#define ICM20608_INT_DATA_RDY_EN	0x01
#define ICM20608_CONFIG_FIFO_MODE	0x40	/* FIFO 满了不再写入：不覆盖旧数据，样本边界不错位 */
#define ICM20608_USER_FIFO_EN		0x40
#define ICM20608_USER_FIFO_RST		0x04	/* 写 1 复位 FIFO，自动清零 */
#define ICM20608_FIFO_EN_ALL		0xF8	/* TEMP | XG | YG | ZG | ACCEL */

/*
 * FIFO 里样本的字节顺序与 0x3B 起的数据寄存器相同（加速度、温度、陀螺仪），
 * 所以缓冲区记录格式和 v3 完全一样，用户态不用改。
 */
#define ICM20608_FIFO_SIZE		512
#define ICM20608_SAMPLE_BYTES		14
#define ICM20608_FIFO_SAMPLES		(ICM20608_FIFO_SIZE / ICM20608_SAMPLE_BYTES)	/* 36 */
#define ICM20608_FIFO_WM_MAX		24	/* 每批最多 24 个，留 12 个样本的余量给线程迟到 */

/* 采样率 = 1 kHz / (1 + SMPLRT_DIV)（DLPF 打开时内部采样 1 kHz） */
#define ICM20608_INTERNAL_RATE_HZ	1000
#define ICM20608_MIN_RATE_HZ		4	/* SMPLRT_DIV 最大 255 -> 3.9 Hz */

/*
 * IIO 的换算系数（与上面固定的量程对应）：
 *   加速度 9.80665 / 16384            = 0.000598550 m/s^2 每 LSB
 *   角速度 (1 / 16.4) * PI / 180      = 0.001064225 rad/s 每 LSB
 *   温度   1000 / 326.8               = 3.059976    毫摄氏度每 LSB
 *          offset = 25 degC * 326.8   = 8170 LSB
 */
#define ICM20608_ACCEL_SCALE_NANO	598550
#define ICM20608_GYRO_SCALE_NANO	1064225
#define ICM20608_TEMP_SCALE_INT		3
#define ICM20608_TEMP_SCALE_MICRO	59976
#define ICM20608_TEMP_OFFSET		8170

struct icm20608_chip {
	const char *name;	/* 打印用 */
	const char *iio_name;	/* IIO 设备名，出现在 .../name */
	u8 whoami;
};

static const struct icm20608_chip icm20608g = { "ICM-20608-G", "icm20608", 0xAF };
static const struct icm20608_chip icm20608d = { "ICM-20608-D", "icm20608d", 0xAE };

static const struct of_device_id icm20608_of_match[] = {
	{ .compatible = "invensense,icm20608",  .data = &icm20608g },
	{ .compatible = "invensense,icm20608d", .data = &icm20608d },
	{ }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

/* 每个设备一份的状态，由 devm_iio_device_alloc 一起分配（iio_priv 取得） */
struct icm20608_dev {
	struct spi_device *spi;
	struct regmap *map;
	const struct icm20608_chip *chip;
	struct iio_trigger *trig;	/* 数据就绪触发器（没有中断时为 NULL） */
	struct mutex lock;		/* 保护采样率配置；v5 还保护 FIFO 读取和下面的推算状态 */
	u8 smplrt_div;

	/* v5 硬件 FIFO */
	bool use_fifo;
	bool pump_rt;			/* 消息泵线程是本驱动改成实时的，卸载时改回 */
	struct task_struct *fifo_task;	/* 定时读 FIFO 的实时线程，只在缓冲区打开期间存在 */
	unsigned int fifo_wm;		/* 每批样本数 = hwfifo_watermark */
	u64 fifo_interval_ns;		/* 线程周期 = fifo_wm 个标称采样周期 */
	unsigned int fifo_overflows;
	u8 *fifo_cmd;			/* SPI 收发缓冲区用 kmalloc 分配：SPI 控制器可能用 DMA，*/
	u8 *fifo_buf;			/* 不能用栈上或与其他字段共用缓存行的内存 */
	/* 时间戳推算（CLOCK_MONOTONIC，单位 ns） */
	s64 nominal_ns;			/* 标称采样周期 */
	s64 period_ns;			/* 芯片实际输出周期的估计 */
	s64 ts_last;			/* 上一个推出的样本 */
	s64 ts_t0;			/* 周期估计的起点 */
	u64 ts_count;			/* 起点以来的样本数 */
	/*
	 * 推给缓冲区的一条记录：14 字节数据 + 2 字节填充 + 8 字节时间戳。
	 * 时间戳必须 8 字节对齐，iio_push_to_buffers_with_timestamp 会把它
	 * 写在 data 的第 16 字节处，所以整块要 24 字节、按 8 对齐。
	 */
	u8 scan[24] __aligned(8);
};

/* 缓冲区里各通道的顺序 = 芯片寄存器里的顺序，一次连续读就能填满 */
enum icm20608_scan {
	ICM20608_SCAN_ACCEL_X,
	ICM20608_SCAN_ACCEL_Y,
	ICM20608_SCAN_ACCEL_Z,
	ICM20608_SCAN_TEMP,
	ICM20608_SCAN_GYRO_X,
	ICM20608_SCAN_GYRO_Y,
	ICM20608_SCAN_GYRO_Z,
	ICM20608_SCAN_TIMESTAMP,
};

/*
 * regmap：告诉内核这颗芯片的寄存器格式，之后统一用 regmap_read/write，
 * 不用再自己拼 SPI 字节。读的时候地址最高位置 1（read_flag_mask）。
 */
static const struct regmap_config icm20608_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.read_flag_mask = 0x80,
	.max_register = ICM20608_REG_WHO_AM_I,
};

/* ---------- IIO 通道：每个通道对应 sysfs 下的一组文件 ---------- */

/* 缓冲区里每个数据通道：有符号 16 位，大端（芯片原样），由用户态转换 */
#define ICM20608_SCAN_TYPE {						\
	.sign = 's',							\
	.realbits = 16,							\
	.storagebits = 16,						\
	.endianness = IIO_BE,						\
}

#define ICM20608_AXIS_CHAN(_type, _axis, _addr, _si) {			\
	.type = _type,							\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.address = _addr,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.scan_index = _si,						\
	.scan_type = ICM20608_SCAN_TYPE,				\
}

static const struct iio_chan_spec icm20608_channels[] = {
	ICM20608_AXIS_CHAN(IIO_ACCEL, X, ICM20608_REG_ACCEL_XOUT_H, ICM20608_SCAN_ACCEL_X),
	ICM20608_AXIS_CHAN(IIO_ACCEL, Y, ICM20608_REG_ACCEL_YOUT_H, ICM20608_SCAN_ACCEL_Y),
	ICM20608_AXIS_CHAN(IIO_ACCEL, Z, ICM20608_REG_ACCEL_ZOUT_H, ICM20608_SCAN_ACCEL_Z),
	{
		.type = IIO_TEMP,
		.address = ICM20608_REG_TEMP_OUT_H,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = ICM20608_SCAN_TEMP,
		.scan_type = ICM20608_SCAN_TYPE,
	},
	ICM20608_AXIS_CHAN(IIO_ANGL_VEL, X, ICM20608_REG_GYRO_XOUT_H, ICM20608_SCAN_GYRO_X),
	ICM20608_AXIS_CHAN(IIO_ANGL_VEL, Y, ICM20608_REG_GYRO_YOUT_H, ICM20608_SCAN_GYRO_Y),
	ICM20608_AXIS_CHAN(IIO_ANGL_VEL, Z, ICM20608_REG_GYRO_ZOUT_H, ICM20608_SCAN_GYRO_Z),
	IIO_CHAN_SOFT_TIMESTAMP(ICM20608_SCAN_TIMESTAMP),
};

/*
 * 每次都读全部 7 个数据通道（一次 SPI 连续读最省事），用户只打开其中
 * 一部分时，由 IIO 核心按这张表从整条记录里挑出需要的通道。
 */
static const unsigned long icm20608_scan_masks[] = {
	BIT(ICM20608_SCAN_ACCEL_X) | BIT(ICM20608_SCAN_ACCEL_Y) |
	BIT(ICM20608_SCAN_ACCEL_Z) | BIT(ICM20608_SCAN_TEMP) |
	BIT(ICM20608_SCAN_GYRO_X) | BIT(ICM20608_SCAN_GYRO_Y) |
	BIT(ICM20608_SCAN_GYRO_Z),
	0,
};

/* 用户 cat 某个 sysfs 文件时，IIO 核心调用这里，mask 说明要读的是哪种值 */
static int icm20608_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct icm20608_dev *idev = iio_priv(indio_dev);
	__be16 data;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/* 缓冲区在跑时，数据归缓冲区，不允许再单次读 */
		if (iio_buffer_enabled(indio_dev))
			return -EBUSY;
		/* 每个量 2 字节、高字节在前，一次连续读 */
		ret = regmap_bulk_read(idev->map, chan->address, &data, sizeof(data));
		if (ret)
			return ret;
		*val = (s16)be16_to_cpu(data);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			*val = 0;
			*val2 = ICM20608_ACCEL_SCALE_NANO;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_ANGL_VEL:
			*val = 0;
			*val2 = ICM20608_GYRO_SCALE_NANO;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_TEMP:
			*val = ICM20608_TEMP_SCALE_INT;
			*val2 = ICM20608_TEMP_SCALE_MICRO;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		if (chan->type != IIO_TEMP)
			return -EINVAL;
		*val = ICM20608_TEMP_OFFSET;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = ICM20608_INTERNAL_RATE_HZ / (1 + idev->smplrt_div);
		return IIO_VAL_INT;
	}

	return -EINVAL;
}

/* echo 200 > sampling_frequency：换算成 SMPLRT_DIV 写进芯片 */
static int icm20608_write_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int val, int val2, long mask)
{
	struct icm20608_dev *idev = iio_priv(indio_dev);
	int div, ret;

	if (mask != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;
	if (val < ICM20608_MIN_RATE_HZ || val > ICM20608_INTERNAL_RATE_HZ || val2)
		return -EINVAL;
	if (iio_buffer_enabled(indio_dev))
		return -EBUSY;

	div = ICM20608_INTERNAL_RATE_HZ / val - 1;

	mutex_lock(&idev->lock);
	ret = regmap_write(idev->map, ICM20608_REG_SMPLRT_DIV, div);
	if (!ret)
		idev->smplrt_div = div;
	mutex_unlock(&idev->lock);
	return ret;
}

static const struct iio_info icm20608_info = {
	.driver_module = THIS_MODULE,
	.read_raw = icm20608_read_raw,
	.write_raw = icm20608_write_raw,
};

/* ---------- 缓冲区 + 数据就绪触发器 ---------- */

/*
 * 下半部：在内核线程里运行，可以睡眠，所以 SPI 读放在这里。
 * pf->timestamp 是上半部 iio_pollfunc_store_time 记下的中断时刻。
 */
static irqreturn_t icm20608_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct icm20608_dev *idev = iio_priv(indio_dev);
	int ret;

	ret = regmap_bulk_read(idev->map, ICM20608_REG_ACCEL_XOUT_H,
			       idev->scan, 14);
	if (!ret)
		iio_push_to_buffers_with_timestamp(indio_dev, idev->scan,
						   pf->timestamp);

	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

/* 用户打开/关闭缓冲区时，IIO 核心调用这里开关芯片的数据就绪中断 */
static int icm20608_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct icm20608_dev *idev = iio_priv(indio_dev);

	return regmap_write(idev->map, ICM20608_REG_INT_ENABLE,
			    state ? ICM20608_INT_DATA_RDY_EN : 0);
}

static const struct iio_trigger_ops icm20608_trigger_ops = {
	.owner = THIS_MODULE,
	.set_trigger_state = icm20608_set_trigger_state,
};

static int icm20608_trigger_init(struct iio_dev *indio_dev)
{
	struct icm20608_dev *idev = iio_priv(indio_dev);
	struct spi_device *spi = idev->spi;
	int ret;

	idev->trig = devm_iio_trigger_alloc(&spi->dev, "%s-dev%d",
					    indio_dev->name, indio_dev->id);
	if (!idev->trig)
		return -ENOMEM;

	/*
	 * 上半部：iio_trigger_generic_data_rdy_poll 运行在硬中断上下文，
	 * 只调用 iio_trigger_poll() 通知触发器，不碰 SPI。触发方式（上升沿）
	 * 来自设备树的 interrupts 属性，所以 flags 传 0。
	 */
	ret = devm_request_irq(&spi->dev, spi->irq,
			       iio_trigger_generic_data_rdy_poll, 0,
			       "icm20608-drdy", idev->trig);
	if (ret) {
		dev_err(&spi->dev, "request irq %d failed: %d\n", spi->irq, ret);
		return ret;
	}

	idev->trig->dev.parent = &spi->dev;
	idev->trig->ops = &icm20608_trigger_ops;
	iio_trigger_set_drvdata(idev->trig, indio_dev);

	ret = iio_trigger_register(idev->trig);
	if (ret)
		return ret;

	/* 默认就用这个触发器，用户不用再手动设置 current_trigger */
	indio_dev->trig = iio_trigger_get(idev->trig);
	return 0;
}

/* ---------- v5：硬件 FIFO + 定时读取线程 ---------- */

/* 复位并重新打开 FIFO（顺序同主线 inv_reset_fifo：先停写入，再复位，再打开） */
static int icm20608_fifo_reset(struct icm20608_dev *idev)
{
	int ret;

	ret = regmap_write(idev->map, ICM20608_REG_FIFO_EN, 0);
	if (!ret)
		ret = regmap_write(idev->map, ICM20608_REG_USER_CTRL,
				   ICM20608_USER_I2C_IF_DIS | ICM20608_USER_FIFO_RST);
	if (!ret)
		ret = regmap_write(idev->map, ICM20608_REG_USER_CTRL,
				   ICM20608_USER_I2C_IF_DIS | ICM20608_USER_FIFO_EN);
	if (!ret)
		ret = regmap_write(idev->map, ICM20608_REG_FIFO_EN, ICM20608_FIFO_EN_ALL);
	if (ret)
		return ret;

	/* FIFO 从现在开始收样本：周期估计重新起算，已有的周期估计保留 */
	idev->ts_t0 = ktime_get_ns();
	idev->ts_last = idev->ts_t0;
	idev->ts_count = 0;
	return 0;
}

static int icm20608_fifo_stop(struct icm20608_dev *idev)
{
	int ret;

	ret = regmap_write(idev->map, ICM20608_REG_FIFO_EN, 0);
	if (!ret)
		ret = regmap_write(idev->map, ICM20608_REG_USER_CTRL,
				   ICM20608_USER_I2C_IF_DIS | ICM20608_USER_FIFO_RST);
	return ret;
}

/* 一次 SPI 传输读出 len 字节 FIFO 数据：先发地址（读标志位），片选保持，再连续收 */
static int icm20608_fifo_read(struct icm20608_dev *idev, unsigned int len)
{
	struct spi_transfer xfers[2] = {
		{ .tx_buf = idev->fifo_cmd, .len = 1 },
		{ .rx_buf = idev->fifo_buf, .len = len },
	};

	return spi_sync_transfer(idev->spi, xfers, ARRAY_SIZE(xfers));
}

/*
 * 给一批 n 个样本推算时间戳，返回相邻样本的间隔。
 * t0、t1 是读 FIFO_COUNT 前后的时刻：芯片锁存计数的那一刻一定在两者之间。
 *
 * 一批样本只知道"读的时候 FIFO 里有 n 个"，不知道每个是哪一刻采的。已知两点：
 *   1. 样本间隔是芯片自己的输出周期，和标称值（1 kHz / (1 + SMPLRT_DIV)）有偏差
 *      （v4 实测快约 0.09%）。长期看它 = 经过的时间 / 样本数，误差小于 1 个周期 / 样本数，
 *      所以越久越准；
 *   2. 最新的样本一定采于 (t0 - 周期, t1] 内：不晚于锁存计数，也不会早于锁存前一个周期，
 *      否则锁存时 FIFO 里应该已经有下一个了。
 * 做法：按估计周期接着上一批往后排 n 个；排出来的最后一个如果落在这个窗口外，就把这一批
 * 均匀拉伸或压缩到窗口边上。时间戳单调、间隔平滑，误差不超过 1 个周期，也不随时间累积。
 * （只用相邻两次读取的时刻做线性插值、像 bmc150 那样，每批 n 会在 9、10、11 之间跳，
 *  间隔就跟着有 ±10% 的锯齿。）
 * 窗口必须用读计数前后两个时刻：只用读完后的时刻，线程如果恰好在读完计数、取时间之前
 * 被更高优先级的任务抢占，这个时刻就晚了，整批会被错误地拉长（实测：实时任务每秒占用
 * 20 ms 时，FIFO 没有溢出，却统计出 24 个"丢失"的样本）。
 * 溢出时 FIFO 里是最早的 36 个样本，之后芯片停写了一段，最新样本不在窗口里，只用上界。
 */
static s64 icm20608_fifo_step(struct icm20608_dev *idev, unsigned int n,
			      s64 t0, s64 t1, bool overflow)
{
	s64 tol = div_s64(idev->nominal_ns, 20);	/* 32 位 ARM 上 64 位除法要用 div_s64 */
	s64 p = idev->period_ns;
	s64 last;

	idev->ts_count += n;
	if (idev->ts_count >= 64) {
		p = div64_u64(t1 - idev->ts_t0, idev->ts_count);
		/* 起点刚复位或时钟异常时不让估计跑偏：芯片时钟偏差远小于 5% */
		p = clamp_t(s64, p, idev->nominal_ns - tol, idev->nominal_ns + tol);
		idev->period_ns = p;
	}

	last = idev->ts_last + n * p;
	if (last > t1)
		last = t1;
	if (!overflow && last < t0 - p)
		last = t0 - p;
	/*
	 * 间隔恒为正：排出来的 ts_last + n * p 和窗口上界 t1 都大于 ts_last
	 * （ts_last 不超过上一次的 t1），下界只会把 last 往大推
	 */
	return div_s64(last - idev->ts_last, n);
}

/* 读走 FIFO 里的全部完整样本推进缓冲区；调用者持有 idev->lock */
static int icm20608_fifo_drain(struct iio_dev *indio_dev)
{
	struct icm20608_dev *idev = iio_priv(indio_dev);
	unsigned int count, n, i;
	bool overflow;
	__be16 cnt;
	s64 t0, t1, off, step, ts;
	int ret;

	/*
	 * 推算在单调时钟上做，不受 NTP 校时跳变影响；推给用户的时间戳与 v3 一样用
	 * iio_get_time_ns()（4.1 为 CLOCK_REALTIME），在这里换算
	 */
	t0 = ktime_get_ns();
	ret = regmap_bulk_read(idev->map, ICM20608_REG_FIFO_COUNTH, &cnt, sizeof(cnt));
	if (ret)
		return ret;
	t1 = ktime_get_ns();
	off = ktime_get_real_ns() - t1;

	count = be16_to_cpu(cnt);
	/* 放不下下一个完整样本：已经（或马上）溢出，芯片停写，读完现有的就复位 */
	overflow = count > ICM20608_FIFO_SIZE - ICM20608_SAMPLE_BYTES;
	/* 只读完整样本；不足 14 字节的尾巴是芯片正在写的样本，留到下次 */
	n = min_t(unsigned int, count / ICM20608_SAMPLE_BYTES, ICM20608_FIFO_SAMPLES);

	if (n) {
		ret = icm20608_fifo_read(idev, n * ICM20608_SAMPLE_BYTES);
		if (ret)
			return ret;
		step = icm20608_fifo_step(idev, n, t0, t1, overflow);
		ts = idev->ts_last;
		for (i = 0; i < n; i++) {
			ts += step;
			/* 推送会在记录末尾写时间戳，所以先拷进 24 字节、8 字节对齐的 scan */
			memcpy(idev->scan, idev->fifo_buf + i * ICM20608_SAMPLE_BYTES,
			       ICM20608_SAMPLE_BYTES);
			iio_push_to_buffers_with_timestamp(indio_dev, idev->scan, ts + off);
		}
		idev->ts_last = ts;
	}

	if (overflow) {
		idev->fifo_overflows++;
		dev_warn_ratelimited(&idev->spi->dev, "FIFO overflow #%u, reset\n",
				     idev->fifo_overflows);
		ret = icm20608_fifo_reset(idev);
	}
	return ret;
}

/*
 * 定时读取线程：代替芯片没有的 FIFO 水位中断。绝对时间定时，不会因为每次处理的耗时
 * 而逐渐变慢；被高优先级任务拖住超过一个周期，就从当前时刻重新计时，不连续补跑
 * （FIFO 里攒下的样本下一次一起读走）。
 */
static int icm20608_fifo_thread(void *arg)
{
	struct iio_dev *indio_dev = arg;
	struct icm20608_dev *idev = iio_priv(indio_dev);
	ktime_t next = ktime_get();
	int ret;

	while (!kthread_should_stop()) {
		next = ktime_add_ns(next, idev->fifo_interval_ns);
		set_current_state(TASK_INTERRUPTIBLE);
		/* kthread_stop() 先置标志再唤醒，检查之后才睡，不会错过 */
		if (!kthread_should_stop())
			schedule_hrtimeout(&next, HRTIMER_MODE_ABS);
		__set_current_state(TASK_RUNNING);
		if (kthread_should_stop())
			break;

		mutex_lock(&idev->lock);
		ret = icm20608_fifo_drain(indio_dev);
		mutex_unlock(&idev->lock);
		if (ret)
			dev_err_ratelimited(&idev->spi->dev, "FIFO read failed: %d\n", ret);

		if (ktime_compare(ktime_get(), next) > 0)
			next = ktime_get();
	}
	return 0;
}

/* 缓冲区打开后：复位并打开 FIFO，启动读取线程 */
static int icm20608_fifo_postenable(struct iio_dev *indio_dev)
{
	/* 与线程化中断处理函数相同的实时优先级（SCHED_FIFO 50） */
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct icm20608_dev *idev = iio_priv(indio_dev);
	struct task_struct *task;
	int ret;

	mutex_lock(&idev->lock);
	idev->nominal_ns = (s64)NSEC_PER_SEC / ICM20608_INTERNAL_RATE_HZ *
			   (1 + idev->smplrt_div);
	idev->period_ns = idev->nominal_ns;
	idev->fifo_interval_ns = (u64)idev->fifo_wm * idev->nominal_ns;
	ret = icm20608_fifo_reset(idev);
	mutex_unlock(&idev->lock);
	if (ret)
		return ret;

	task = kthread_run(icm20608_fifo_thread, indio_dev, "icm20608-fifo");
	if (IS_ERR(task)) {
		mutex_lock(&idev->lock);
		icm20608_fifo_stop(idev);
		mutex_unlock(&idev->lock);
		return PTR_ERR(task);
	}
	sched_setscheduler(task, SCHED_FIFO, &param);
	idev->fifo_task = task;
	return 0;
}

/* 缓冲区关闭前：先停线程（之后不会再有人读 FIFO），再关 FIFO */
static int icm20608_fifo_predisable(struct iio_dev *indio_dev)
{
	struct icm20608_dev *idev = iio_priv(indio_dev);
	int ret;

	if (idev->fifo_task) {
		kthread_stop(idev->fifo_task);
		idev->fifo_task = NULL;
	}
	mutex_lock(&idev->lock);
	ret = icm20608_fifo_stop(idev);
	mutex_unlock(&idev->lock);
	return ret;
}

static const struct iio_buffer_setup_ops icm20608_fifo_setup_ops = {
	.postenable = icm20608_fifo_postenable,
	.predisable = icm20608_fifo_predisable,
};

/*
 * 用户写 buffer/watermark 时 IIO 核心调用（缓冲区关闭时才允许写）：
 * 软件缓冲区攒够 watermark 个才唤醒 poll/read，硬件这边就每 watermark 个周期读一批，
 * 两者对齐，读者每批只被唤醒一次。超过 FIFO 安全上限时硬件这边取上限。
 */
static int icm20608_fifo_set_watermark(struct iio_dev *indio_dev, unsigned int val)
{
	struct icm20608_dev *idev = iio_priv(indio_dev);

	mutex_lock(&idev->lock);
	idev->fifo_wm = clamp_t(unsigned int, val, 1, ICM20608_FIFO_WM_MAX);
	mutex_unlock(&idev->lock);
	return 0;
}

/* buffer/ 下的 hwfifo_* 属性，名字和含义见内核 Documentation/ABI/testing/sysfs-bus-iio */
static ssize_t icm20608_hwfifo_watermark_show(struct device *dev,
					      struct device_attribute *attr, char *buf)
{
	struct icm20608_dev *idev = iio_priv(dev_to_iio_dev(dev));

	return sprintf(buf, "%u\n", idev->fifo_wm);
}

static ssize_t icm20608_hwfifo_enabled_show(struct device *dev,
					    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", iio_buffer_enabled(dev_to_iio_dev(dev)));
}

static IIO_CONST_ATTR(hwfifo_watermark_min, "1");
static IIO_CONST_ATTR(hwfifo_watermark_max, __stringify(ICM20608_FIFO_WM_MAX));
static IIO_DEVICE_ATTR(hwfifo_watermark, S_IRUGO, icm20608_hwfifo_watermark_show, NULL, 0);
static IIO_DEVICE_ATTR(hwfifo_enabled, S_IRUGO, icm20608_hwfifo_enabled_show, NULL, 0);

static const struct attribute *icm20608_fifo_buffer_attrs[] = {
	&iio_const_attr_hwfifo_watermark_min.dev_attr.attr,
	&iio_const_attr_hwfifo_watermark_max.dev_attr.attr,
	&iio_dev_attr_hwfifo_watermark.dev_attr.attr,
	&iio_dev_attr_hwfifo_enabled.dev_attr.attr,
	NULL,
};

/* 设备目录下的 fifo_overflows：加载以来 FIFO 溢出（复位）次数 */
static ssize_t icm20608_fifo_overflows_show(struct device *dev,
					    struct device_attribute *attr, char *buf)
{
	struct icm20608_dev *idev = iio_priv(dev_to_iio_dev(dev));

	return sprintf(buf, "%u\n", idev->fifo_overflows);
}

static IIO_DEVICE_ATTR(fifo_overflows, S_IRUGO, icm20608_fifo_overflows_show, NULL, 0);

static struct attribute *icm20608_fifo_attrs[] = {
	&iio_dev_attr_fifo_overflows.dev_attr.attr,
	NULL,
};

static const struct attribute_group icm20608_fifo_attr_group = {
	.attrs = icm20608_fifo_attrs,
};

static const struct iio_info icm20608_fifo_info = {
	.driver_module = THIS_MODULE,
	.read_raw = icm20608_read_raw,
	.write_raw = icm20608_write_raw,
	.attrs = &icm20608_fifo_attr_group,
	.hwfifo_set_watermark = icm20608_fifo_set_watermark,
};

/* kfifo 软件缓冲区 + 上面的开关回调；没有触发器，数据由读取线程直接推入 */
static int icm20608_fifo_setup(struct iio_dev *indio_dev)
{
	struct icm20608_dev *idev = iio_priv(indio_dev);
	struct device *dev = &idev->spi->dev;
	struct iio_buffer *buffer;

	idev->fifo_cmd = devm_kmalloc(dev, 1, GFP_KERNEL);
	idev->fifo_buf = devm_kmalloc(dev, ICM20608_FIFO_SIZE, GFP_KERNEL);
	buffer = devm_iio_kfifo_allocate(dev);
	if (!idev->fifo_cmd || !idev->fifo_buf || !buffer)
		return -ENOMEM;
	idev->fifo_cmd[0] = ICM20608_REG_FIFO_R_W | 0x80;	/* 最高位 1 = 读 */
	idev->fifo_wm = 1;	/* 与 IIO 缓冲区 watermark 的默认值一致 */

	buffer->attrs = icm20608_fifo_buffer_attrs;
	iio_device_attach_buffer(indio_dev, buffer);
	indio_dev->modes |= INDIO_BUFFER_SOFTWARE;
	indio_dev->setup_ops = &icm20608_fifo_setup_ops;
	return 0;
}

/* ---------- 芯片初始化 ---------- */

struct icm20608_reg_val {
	u8 reg;
	u8 val;
};

static int icm20608_chip_init(struct icm20608_dev *idev)
{
	static const struct icm20608_reg_val cfg[] = {
		{ ICM20608_REG_USER_CTRL,     ICM20608_USER_I2C_IF_DIS },
		{ ICM20608_REG_PWR_MGMT_2,    0x00 },	/* 6 个轴全开 */
		{ ICM20608_REG_SMPLRT_DIV,    0x00 },	/* 1 kHz / (1 + 0) */
		{ ICM20608_REG_CONFIG,        ICM20608_DLPF_CFG | ICM20608_CONFIG_FIFO_MODE },
		{ ICM20608_REG_GYRO_CONFIG,   ICM20608_GYRO_FS_2000DPS },
		{ ICM20608_REG_ACCEL_CONFIG,  ICM20608_ACCEL_FS_2G },
		{ ICM20608_REG_ACCEL_CONFIG2, ICM20608_A_DLPF_CFG },
		{ ICM20608_REG_INT_PIN_CFG,   ICM20608_INT_PIN_CFG_DEFAULT },
		{ ICM20608_REG_INT_ENABLE,    0x00 },	/* 缓冲区打开时才开中断 */
	};
	struct device *dev = &idev->spi->dev;
	unsigned int val;
	int i, ret;

	/* 1. 软件复位；手册：复位后最长 100 ms 才能读写寄存器 */
	ret = regmap_write(idev->map, ICM20608_REG_PWR_MGMT_1,
			   ICM20608_PWR1_DEVICE_RESET);
	if (ret)
		return ret;
	msleep(100);

	/* 2. 先唤醒：复位后芯片处于睡眠，睡眠时部分寄存器（如 USER_CTRL）
	 *    写不进去。手册：从睡眠到可读写最长 5 ms */
	ret = regmap_write(idev->map, ICM20608_REG_PWR_MGMT_1,
			   ICM20608_PWR1_CLKSEL_AUTO);
	if (ret)
		return ret;
	msleep(10);

	/* 3. 逐个写配置，并读回校验 */
	for (i = 0; i < ARRAY_SIZE(cfg); i++) {
		ret = regmap_write(idev->map, cfg[i].reg, cfg[i].val);
		if (ret)
			return ret;
		ret = regmap_read(idev->map, cfg[i].reg, &val);
		if (ret)
			return ret;
		if (val != cfg[i].val) {
			dev_err(dev, "reg 0x%02x: wrote 0x%02x, read 0x%02x\n",
				cfg[i].reg, cfg[i].val, val);
			return -EIO;
		}
	}

	/* 4. 陀螺仪从睡眠启动需要约 35 ms 才稳定 */
	msleep(50);
	return 0;
}

/* ---------- SPI 消息泵优先级（见模块参数 spi_rt） ---------- */

static void icm20608_spi_pump_rt(struct icm20608_dev *idev, bool on)
{
	struct task_struct *pump = idev->spi->master->kworker_task;
	struct sched_param rt = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct sched_param normal = { .sched_priority = 0 };

	if (!pump)	/* 控制器驱动自己管理队列，没有核心的消息泵 */
		return;
	if (on) {
		/* 已经是实时的（控制器设置了 master->rt 或用户 chrt 过）就不动 */
		if (pump->policy != SCHED_NORMAL || sched_setscheduler(pump, SCHED_FIFO, &rt))
			return;
		idev->pump_rt = true;
		dev_info(&idev->spi->dev, "SPI message pump [%s] set to SCHED_FIFO %d\n",
			 pump->comm, rt.sched_priority);
	} else if (idev->pump_rt) {
		sched_setscheduler(pump, SCHED_NORMAL, &normal);
		idev->pump_rt = false;
	}
}

/* ---------- probe / remove ---------- */

static int icm20608_probe(struct spi_device *spi)
{
	const struct of_device_id *match;
	struct iio_dev *indio_dev;
	struct icm20608_dev *idev;
	unsigned int id;
	int ret;

	match = of_match_device(icm20608_of_match, &spi->dev);
	if (!match)
		return -ENODEV;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	/* IIO 设备和我们自己的结构体一次分配；devm_：解绑时自动释放 */
	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*idev));
	if (!indio_dev)
		return -ENOMEM;
	idev = iio_priv(indio_dev);
	idev->spi = spi;
	idev->chip = match->data;
	mutex_init(&idev->lock);

	idev->map = devm_regmap_init_spi(spi, &icm20608_regmap_config);
	if (IS_ERR(idev->map)) {
		dev_err(&spi->dev, "regmap init failed\n");
		return PTR_ERR(idev->map);
	}

	ret = regmap_read(idev->map, ICM20608_REG_WHO_AM_I, &id);
	if (ret) {
		dev_err(&spi->dev, "read WHO_AM_I failed: %d\n", ret);
		return ret;
	}
	if (id != idev->chip->whoami) {
		dev_err(&spi->dev, "WHO_AM_I = 0x%02x, expect 0x%02x for %s\n",
			id, idev->chip->whoami, idev->chip->name);
		return -ENODEV;
	}

	ret = icm20608_chip_init(idev);
	if (ret) {
		dev_err(&spi->dev, "chip init failed: %d\n", ret);
		return ret;
	}
	idev->smplrt_div = 0;	/* chip_init 里写的是 0 -> 1 kHz */

	idev->use_fifo = fifo;
	indio_dev->dev.parent = &spi->dev;
	indio_dev->name = idev->chip->iio_name;
	indio_dev->info = idev->use_fifo ? &icm20608_fifo_info : &icm20608_info;
	indio_dev->modes = INDIO_DIRECT_MODE;	/* 缓冲区模式由下面的 setup 加上 */
	indio_dev->channels = icm20608_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm20608_channels);
	indio_dev->available_scan_masks = icm20608_scan_masks;

	spi_set_drvdata(spi, indio_dev);

	if (idev->use_fifo) {
		/* v5：不用中断和触发器，资源全是 devm_，失败直接返回 */
		ret = icm20608_fifo_setup(indio_dev);
		if (ret)
			return ret;
		ret = iio_device_register(indio_dev);
		if (ret) {
			dev_err(&spi->dev, "iio_device_register failed: %d\n", ret);
			return ret;
		}
		dev_info(&spi->dev, "%s ready as IIO device %s, hardware FIFO\n",
			 idev->chip->name, dev_name(&indio_dev->dev));
		if (spi_rt)
			icm20608_spi_pump_rt(idev, true);
		return 0;
	}

	/* v3：上半部记时间戳，下半部（线程）读数据 */
	ret = iio_triggered_buffer_setup(indio_dev, iio_pollfunc_store_time,
					 icm20608_trigger_handler, NULL);
	if (ret) {
		dev_err(&spi->dev, "triggered buffer setup failed: %d\n", ret);
		return ret;
	}

	/* 设备树里没写中断时，仍可用 sysfs 单次读，或换用别的触发器 */
	if (spi->irq > 0) {
		ret = icm20608_trigger_init(indio_dev);
		if (ret)
			goto err_buffer;
	} else {
		dev_warn(&spi->dev, "no irq, data-ready trigger disabled\n");
	}

	/* 不用 devm_ 版本：remove 里要先注销 IIO 设备，再让芯片睡眠 */
	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&spi->dev, "iio_device_register failed: %d\n", ret);
		goto err_trigger;
	}

	dev_info(&spi->dev, "%s ready as IIO device %s, irq %d\n",
		 idev->chip->name, dev_name(&indio_dev->dev), spi->irq);
	if (spi_rt)
		icm20608_spi_pump_rt(idev, true);
	return 0;

err_trigger:
	/* indio_dev->trig 的引用由 IIO 核心在释放 iio_dev 时 put，这里不要重复 */
	if (idev->trig)
		iio_trigger_unregister(idev->trig);
err_buffer:
	iio_triggered_buffer_cleanup(indio_dev);
	return ret;
}

static int icm20608_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct icm20608_dev *idev = iio_priv(indio_dev);

	/* 注销时 IIO 核心会先关闭缓冲区（v5 的 predisable 停掉读取线程、关 FIFO） */
	iio_device_unregister(indio_dev);
	if (!idev->use_fifo) {
		/* indio_dev->trig 的引用由 IIO 核心在释放 iio_dev 时 put，这里不要重复 */
		if (idev->trig)
			iio_trigger_unregister(idev->trig);
		iio_triggered_buffer_cleanup(indio_dev);
	}
	icm20608_spi_pump_rt(idev, false);
	regmap_write(idev->map, ICM20608_REG_PWR_MGMT_1, ICM20608_PWR1_SLEEP);
	dev_info(&spi->dev, "removed\n");
	return 0;
}

static const struct spi_device_id icm20608_id[] = {
	{ "icm20608", 0 },
	{ "icm20608d", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, icm20608_id);

static struct spi_driver icm20608_driver = {
	.driver = {
		.name = "icm20608",
		.owner = THIS_MODULE,
		.of_match_table = icm20608_of_match,
	},
	.probe = icm20608_probe,
	.remove = icm20608_remove,
	.id_table = icm20608_id,
};
module_spi_driver(icm20608_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ICM-20608 6-axis IMU SPI/IIO driver");
