// SPDX-License-Identifier: GPL-2.0
/*
 * AP3216C 环境光 + 接近 + 红外三合一传感器的 I2C + IIO 驱动
 *
 * 寄存器访问用 regmap，数据通过 IIO 子系统导出：
 *   /sys/bus/iio/devices/iio:deviceN/in_illuminance_raw    + in_illuminance_scale(_available)
 *   /sys/bus/iio/devices/iio:deviceN/in_proximity_raw      10 位，越大越近
 *   /sys/bus/iio/devices/iio:deviceN/in_intensity_ir_raw   10 位
 * 照度(lux) = in_illuminance_raw * in_illuminance_scale；写 scale 切换 4 档量程。
 * IR 溢出（强光直射）时 IR 与 PS 数据无效，读对应文件返回 -EAGAIN。
 *
 * 读取方式：每个通道一次 I2C 传输读一对寄存器（低字节、高字节）。
 *   实测芯片只在一对寄存器内地址自增，读满 2 字节回到该对开头：从 0x0A 连续读 6 字节
 *   得到 IR,IR,IR 而不是 IR,ALS,PS（数据手册只给了单字节读时序，见 app/ap3216c_i2c_bench.c）。
 *   与逐字节读相比，读一组数据从 6 次传输 / 24 次等待降到 3 次 / 15 次。
 *
 * 接近事件（需要设备树给出 AP_INT 中断，ALPHA 上是 GPIO1_IO01，低电平有效）：
 *   芯片工作在 PS 中断 Mode 2（滞回）：远 -> 近 越过高阈值、近 -> 远 越过低阈值时各触发一次。
 *   events/in_proximity_thresh_rising_{value,en}   远 -> 近，推送 IIO_EV_DIR_RISING
 *   events/in_proximity_thresh_falling_{value,en}  近 -> 远，推送 IIO_EV_DIR_FALLING
 *   上半部（硬中断）记录时间戳；下半部（内核线程）读中断状态和 PS 数据，推送事件，写 1 清中断。
 *   用户态对 /dev/iio:deviceN 做 IIO_GET_EVENT_FD_IOCTL 得到事件 fd，poll/read 即可。
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/events.h>

/* 寄存器地址（AP3216C 数据手册 Register Table） */
#define AP3216C_REG_SYS_CONFIG		0x00
#define AP3216C_REG_INT_STATUS		0x01
#define AP3216C_REG_INT_CLEAR		0x02
#define AP3216C_REG_IR_DATA_LOW		0x0A	/* 0x0A/0x0B 一对 */
#define AP3216C_REG_ALS_DATA_LOW	0x0C	/* 0x0C/0x0D 一对 */
#define AP3216C_REG_PS_DATA_LOW		0x0E	/* 0x0E/0x0F 一对 */
#define AP3216C_REG_ALS_CONFIG		0x10
#define AP3216C_REG_ALS_THR_LOW_L	0x1A	/* 0x1A~0x1D：ALS 低/高阈值 */
#define AP3216C_REG_PS_INT_MODE		0x22
#define AP3216C_REG_PS_THR_LOW_L	0x2A	/* 0x2A~0x2D：PS 低/高阈值 */
#define AP3216C_REG_MAX			0x2D

/* 位定义 */
#define AP3216C_MODE_OFF		0x00
#define AP3216C_MODE_ALS_PS_IR		0x03	/* ALS 与 PS+IR 连续测量 */
#define AP3216C_MODE_RESET		0x04
#define AP3216C_INT_ALS			BIT(0)
#define AP3216C_INT_PS			BIT(1)
#define AP3216C_INT_CLEAR_BY_SW		0x01	/* 写 1 清 INT_STATUS 对应位 */
#define AP3216C_PS_INT_HYSTERESIS	0x01	/* PS 中断 Mode 2 */
#define AP3216C_IR_OVERFLOW		BIT(7)	/* IR 低字节：IR、PS 数据无效 */
#define AP3216C_PS_OBJECT		BIT(7)	/* PS 两个字节：物体靠近 */
#define AP3216C_PS_IR_OVERFLOW		BIT(6)	/* PS 两个字节：IR、PS 数据无效 */
#define AP3216C_ALS_RANGE_SHIFT		4
#define AP3216C_ALS_RANGE_MASK		(3 << AP3216C_ALS_RANGE_SHIFT)
#define AP3216C_PS_MAX			1023	/* 10 位 */

/*
 * ALS 各量程的分辨率（lux/count），取数据手册电气特性表典型值：0.36、0.089、0.022、0.0056。
 * 注意手册寄存器表写的量程是 20661 lux（折合 0.315），与电气特性表 23360 lux 不一致；
 * 电气特性表同时给出 128 lux 时 Range1 计数为 360，与 0.36 吻合，所以采用它。
 */
static const int ap3216c_als_scale_micro[] = { 360000, 89000, 22000, 5600 };

struct ap3216c_data {
	struct i2c_client *client;
	struct regmap *map;
	struct mutex lock;		/* 保护量程、阈值和事件开关 */
	unsigned int als_range;		/* 0~3，对应 ap3216c_als_scale_micro */
	unsigned int ps_rising;		/* 远 -> 近 阈值 */
	unsigned int ps_falling;	/* 近 -> 远 阈值 */
	bool ps_rising_en;
	bool ps_falling_en;
	s64 irq_timestamp;		/* 上半部记录 */
};

static const struct regmap_config ap3216c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AP3216C_REG_MAX,
};

/* ---------- IIO 通道 ---------- */

static const struct iio_event_spec ap3216c_ps_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) | BIT(IIO_EV_INFO_ENABLE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) | BIT(IIO_EV_INFO_ENABLE),
	},
};

#define AP3216C_ALS_CHAN {						\
	.type = IIO_LIGHT,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
			      BIT(IIO_CHAN_INFO_SCALE),			\
	.address = AP3216C_REG_ALS_DATA_LOW,				\
}

#define AP3216C_IR_CHAN {						\
	.type = IIO_INTENSITY,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_LIGHT_IR,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.address = AP3216C_REG_IR_DATA_LOW,				\
}

/* 设备树给了中断才带事件；没有中断时不能导出事件属性（回调为空会出错） */
static const struct iio_chan_spec ap3216c_channels_irq[] = {
	AP3216C_ALS_CHAN,
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.address = AP3216C_REG_PS_DATA_LOW,
		.event_spec = ap3216c_ps_events,
		.num_event_specs = ARRAY_SIZE(ap3216c_ps_events),
	},
	AP3216C_IR_CHAN,
};

static const struct iio_chan_spec ap3216c_channels[] = {
	AP3216C_ALS_CHAN,
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.address = AP3216C_REG_PS_DATA_LOW,
	},
	AP3216C_IR_CHAN,
};

static IIO_CONST_ATTR(in_illuminance_scale_available, "0.36 0.089 0.022 0.0056");

static struct attribute *ap3216c_attributes[] = {
	&iio_const_attr_in_illuminance_scale_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group ap3216c_attribute_group = {
	.attrs = ap3216c_attributes,
};

/* ---------- 寄存器读写 ---------- */

/* 一次 I2C 传输读一对寄存器（芯片只在一对内地址自增） */
static int ap3216c_read_pair(struct ap3216c_data *data, unsigned int reg, u8 buf[2])
{
	return regmap_bulk_read(data->map, reg, buf, 2);
}

/*
 * 阈值寄存器逐个写：块读在一对寄存器内回绕，块写未验证，
 * 而 0x1A~0x1D、0x2A~0x2D 都跨了两对，所以不用 regmap_bulk_write
 */
static int ap3216c_write_regs(struct ap3216c_data *data, unsigned int reg,
			      const u8 *vals, int n)
{
	int i, ret;

	for (i = 0; i < n; i++) {
		ret = regmap_write(data->map, reg + i, vals[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * 按事件开关设置 PS 阈值（PS 阈值 = 高字节 * 4 + 低字节）。
 * 关闭的方向设成永远越不过的值：高阈值 1023、低阈值 0。
 * Mode 2 是状态机：芯片先进入“近”状态（越过高阈值）后才可能触发“远”，
 * 所以只开 falling 不开 rising 时不会有事件。
 */
static int ap3216c_update_ps_thresholds(struct ap3216c_data *data)
{
	unsigned int low = data->ps_falling_en ? data->ps_falling : 0;
	unsigned int high = data->ps_rising_en ? data->ps_rising : AP3216C_PS_MAX;
	const u8 vals[4] = { low & 0x3, low >> 2, high & 0x3, high >> 2 };

	return ap3216c_write_regs(data, AP3216C_REG_PS_THR_LOW_L, vals, 4);
}

static int ap3216c_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ap3216c_data *data = iio_priv(indio_dev);
	u8 buf[2];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ap3216c_read_pair(data, chan->address, buf);
		if (ret)
			return ret;

		switch (chan->type) {
		case IIO_LIGHT:
			*val = (buf[1] << 8) | buf[0];
			return IIO_VAL_INT;
		case IIO_INTENSITY:
			if (buf[0] & AP3216C_IR_OVERFLOW)
				return -EAGAIN;
			*val = (buf[1] << 2) | (buf[0] & 0x03);
			return IIO_VAL_INT;
		case IIO_PROXIMITY:
			if ((buf[0] | buf[1]) & AP3216C_PS_IR_OVERFLOW)
				return -EAGAIN;
			*val = ((buf[1] & 0x3f) << 4) | (buf[0] & 0x0f);
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_SCALE:
		if (chan->type != IIO_LIGHT)
			return -EINVAL;
		mutex_lock(&data->lock);
		*val = 0;
		*val2 = ap3216c_als_scale_micro[data->als_range];
		mutex_unlock(&data->lock);
		return IIO_VAL_INT_PLUS_MICRO;
	}

	return -EINVAL;
}

/* echo 0.022 > in_illuminance_scale：切换 ALS 量程 */
static int ap3216c_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ap3216c_data *data = iio_priv(indio_dev);
	unsigned int i;
	int ret;

	if (mask != IIO_CHAN_INFO_SCALE || chan->type != IIO_LIGHT || val != 0)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ap3216c_als_scale_micro); i++) {
		if (ap3216c_als_scale_micro[i] != val2)
			continue;

		mutex_lock(&data->lock);
		ret = regmap_update_bits(data->map, AP3216C_REG_ALS_CONFIG,
					 AP3216C_ALS_RANGE_MASK,
					 i << AP3216C_ALS_RANGE_SHIFT);
		if (!ret)
			data->als_range = i;
		mutex_unlock(&data->lock);
		return ret;
	}

	return -EINVAL;
}

/* ---------- 接近事件 ---------- */

static int ap3216c_read_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	struct ap3216c_data *data = iio_priv(indio_dev);

	return dir == IIO_EV_DIR_RISING ? data->ps_rising_en : data->ps_falling_en;
}

static int ap3216c_write_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir,
				      int state)
{
	struct ap3216c_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	if (dir == IIO_EV_DIR_RISING)
		data->ps_rising_en = state;
	else
		data->ps_falling_en = state;
	ret = ap3216c_update_ps_thresholds(data);
	mutex_unlock(&data->lock);
	return ret;
}

static int ap3216c_read_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info,
				    int *val, int *val2)
{
	struct ap3216c_data *data = iio_priv(indio_dev);

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;
	*val = dir == IIO_EV_DIR_RISING ? data->ps_rising : data->ps_falling;
	return IIO_VAL_INT;
}

static int ap3216c_write_event_value(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info,
				     int val, int val2)
{
	struct ap3216c_data *data = iio_priv(indio_dev);
	int ret;

	if (info != IIO_EV_INFO_VALUE || val < 0 || val > AP3216C_PS_MAX)
		return -EINVAL;

	mutex_lock(&data->lock);
	if (dir == IIO_EV_DIR_RISING)
		data->ps_rising = val;
	else
		data->ps_falling = val;
	ret = ap3216c_update_ps_thresholds(data);
	mutex_unlock(&data->lock);
	return ret;
}

/* 上半部：只记时间戳，I2C 访问会睡眠，放到线程里 */
static irqreturn_t ap3216c_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct ap3216c_data *data = iio_priv(indio_dev);

	data->irq_timestamp = iio_get_time_ns();
	return IRQ_WAKE_THREAD;
}

/* 下半部：读中断状态，PS 中断时按“物体靠近”位推送方向，写 1 清除（INT 引脚随之释放） */
static irqreturn_t ap3216c_irq_thread(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct ap3216c_data *data = iio_priv(indio_dev);
	unsigned int status;
	u8 ps[2];
	int ret;

	ret = regmap_read(data->map, AP3216C_REG_INT_STATUS, &status);
	if (ret)
		return IRQ_HANDLED;
	status &= AP3216C_INT_ALS | AP3216C_INT_PS;
	if (!status)
		return IRQ_NONE;

	if ((status & AP3216C_INT_PS) && !ap3216c_read_pair(data, AP3216C_REG_PS_DATA_LOW, ps)) {
		/*
		 * 方向先算进变量再传给宏：4.1 的 IIO_EVENT_CODE 展开为 (u64)direction << 48，
		 * 参数没加括号，直接传 a ? b : c 会因运算符优先级拼出错误的事件码
		 */
		enum iio_event_direction dir = (ps[0] & AP3216C_PS_OBJECT) ?
					       IIO_EV_DIR_RISING : IIO_EV_DIR_FALLING;

		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0, IIO_EV_TYPE_THRESH, dir),
			       data->irq_timestamp);
	}

	regmap_write(data->map, AP3216C_REG_INT_STATUS, status);
	return IRQ_HANDLED;
}

static const struct iio_info ap3216c_info_irq = {
	.driver_module = THIS_MODULE,
	.read_raw = ap3216c_read_raw,
	.write_raw = ap3216c_write_raw,
	.read_event_config = ap3216c_read_event_config,
	.write_event_config = ap3216c_write_event_config,
	.read_event_value = ap3216c_read_event_value,
	.write_event_value = ap3216c_write_event_value,
	.attrs = &ap3216c_attribute_group,
};

static const struct iio_info ap3216c_info = {
	.driver_module = THIS_MODULE,
	.read_raw = ap3216c_read_raw,
	.write_raw = ap3216c_write_raw,
	.attrs = &ap3216c_attribute_group,
};

/* ---------- 初始化 ---------- */

static int ap3216c_chip_init(struct ap3216c_data *data)
{
	/* ALS 阈值设成 [0, 0xFFFF]，ALS 中断永不触发（芯片没有单独的中断使能位） */
	static const u8 als_thr[4] = { 0x00, 0x00, 0xff, 0xff };
	unsigned int mode;
	int ret;

	ret = regmap_write(data->map, AP3216C_REG_SYS_CONFIG, AP3216C_MODE_RESET);
	if (ret)
		return ret;
	msleep(50);

	ret = regmap_write(data->map, AP3216C_REG_INT_CLEAR, AP3216C_INT_CLEAR_BY_SW);
	if (ret)
		return ret;
	ret = ap3216c_write_regs(data, AP3216C_REG_ALS_THR_LOW_L, als_thr, 4);
	if (ret)
		return ret;
	ret = regmap_write(data->map, AP3216C_REG_PS_INT_MODE, AP3216C_PS_INT_HYSTERESIS);
	if (ret)
		return ret;
	ret = ap3216c_update_ps_thresholds(data);
	if (ret)
		return ret;
	/* 清掉复位前可能残留的中断状态 */
	ret = regmap_write(data->map, AP3216C_REG_INT_STATUS,
			   AP3216C_INT_ALS | AP3216C_INT_PS);
	if (ret)
		return ret;

	ret = regmap_write(data->map, AP3216C_REG_SYS_CONFIG, AP3216C_MODE_ALS_PS_IR);
	if (ret)
		return ret;

	/* 芯片没有 ID 寄存器，回读模式确认芯片在位 */
	ret = regmap_read(data->map, AP3216C_REG_SYS_CONFIG, &mode);
	if (ret)
		return ret;
	return mode == AP3216C_MODE_ALS_PS_IR ? 0 : -ENODEV;
}

static int ap3216c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct iio_dev *indio_dev;
	struct ap3216c_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	mutex_init(&data->lock);
	data->als_range = 0;
	/*
	 * 默认阈值按 ALPHA 板实测：无遮挡 PS 36~97（中位 63），手掌距芯片 1~2 cm 时 126~204。
	 * 量程虽是 0~1023，默认增益与 LED 电流下实际只用到低段；不同安装环境用 sysfs 调整
	 */
	data->ps_rising = 140;
	data->ps_falling = 100;

	data->map = devm_regmap_init_i2c(client, &ap3216c_regmap_config);
	if (IS_ERR(data->map))
		return PTR_ERR(data->map);

	ret = ap3216c_chip_init(data);
	if (ret) {
		dev_err(&client->dev, "chip init failed: %d\n", ret);
		return ret;
	}

	i2c_set_clientdata(client, indio_dev);
	indio_dev->dev.parent = &client->dev;
	indio_dev->name = "ap3216c";
	indio_dev->modes = INDIO_DIRECT_MODE;

	if (client->irq > 0) {
		/* 触发方式（低电平）来自设备树；线程处理完清掉中断后才重新打开 */
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						ap3216c_irq_handler, ap3216c_irq_thread,
						IRQF_ONESHOT, "ap3216c", indio_dev);
		if (ret) {
			dev_err(&client->dev, "request irq %d failed: %d\n", client->irq, ret);
			goto err_power_off;
		}
		indio_dev->info = &ap3216c_info_irq;
		indio_dev->channels = ap3216c_channels_irq;
		indio_dev->num_channels = ARRAY_SIZE(ap3216c_channels_irq);
	} else {
		dev_warn(&client->dev, "no irq, proximity events disabled\n");
		indio_dev->info = &ap3216c_info;
		indio_dev->channels = ap3216c_channels;
		indio_dev->num_channels = ARRAY_SIZE(ap3216c_channels);
	}

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&client->dev, "iio_device_register failed: %d\n", ret);
		goto err_power_off;
	}

	dev_info(&client->dev, "ready as IIO device %s, irq %d\n",
		 dev_name(&indio_dev->dev), client->irq);
	return 0;

err_power_off:
	regmap_write(data->map, AP3216C_REG_SYS_CONFIG, AP3216C_MODE_OFF);
	return ret;
}

static int ap3216c_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ap3216c_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	/* 中断由 devm 在 remove 之后释放，先关掉，避免向已注销的设备推事件 */
	if (client->irq > 0)
		disable_irq(client->irq);
	regmap_write(data->map, AP3216C_REG_SYS_CONFIG, AP3216C_MODE_OFF);
	return 0;
}

static const struct of_device_id ap3216c_of_match[] = {
	{ .compatible = "liteon,ap3216c" },
	{ }
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

static const struct i2c_device_id ap3216c_id[] = {
	{ "ap3216c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id);

static struct i2c_driver ap3216c_driver = {
	.driver = {
		.name = "ap3216c",
		.owner = THIS_MODULE,
		.of_match_table = ap3216c_of_match,
	},
	.probe = ap3216c_probe,
	.remove = ap3216c_remove,
	.id_table = ap3216c_id,
};
module_i2c_driver(ap3216c_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AP3216C ambient light / proximity / IR I2C IIO driver");
