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

/* 寄存器地址（ICM-20608 Register Map） */
#define ICM20608_REG_SMPLRT_DIV		0x19
#define ICM20608_REG_CONFIG		0x1A
#define ICM20608_REG_GYRO_CONFIG	0x1B
#define ICM20608_REG_ACCEL_CONFIG	0x1C
#define ICM20608_REG_ACCEL_CONFIG2	0x1D
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
	struct mutex lock;		/* 保护采样率配置 */
	u8 smplrt_div;
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
		{ ICM20608_REG_CONFIG,        ICM20608_DLPF_CFG },
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

	indio_dev->dev.parent = &spi->dev;
	indio_dev->name = idev->chip->iio_name;
	indio_dev->info = &icm20608_info;
	indio_dev->modes = INDIO_DIRECT_MODE;	/* 缓冲区模式由下面的 setup 加上 */
	indio_dev->channels = icm20608_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm20608_channels);
	indio_dev->available_scan_masks = icm20608_scan_masks;

	spi_set_drvdata(spi, indio_dev);

	/* 上半部记时间戳，下半部（线程）读数据 */
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

	iio_device_unregister(indio_dev);
	/* indio_dev->trig 的引用由 IIO 核心在释放 iio_dev 时 put，这里不要重复 */
	if (idev->trig)
		iio_trigger_unregister(idev->trig);
	iio_triggered_buffer_cleanup(indio_dev);
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
