#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>

/* 寄存器地址（ICM-20608 Register Map） */
#define ICM20608_REG_SMPLRT_DIV		0x19
#define ICM20608_REG_CONFIG		0x1A
#define ICM20608_REG_GYRO_CONFIG	0x1B
#define ICM20608_REG_ACCEL_CONFIG	0x1C
#define ICM20608_REG_ACCEL_CONFIG2	0x1D
#define ICM20608_REG_ACCEL_XOUT_H	0x3B	/* 0x3B~0x48 共 14 字节数据 */
#define ICM20608_REG_USER_CTRL		0x6A
#define ICM20608_REG_PWR_MGMT_1		0x6B
#define ICM20608_REG_PWR_MGMT_2		0x6C
#define ICM20608_REG_WHO_AM_I		0x75

/* 位定义 */
#define ICM20608_PWR1_DEVICE_RESET	0x80	/* 复位，完成后自动清零 */
#define ICM20608_PWR1_CLKSEL_AUTO	0x01	/* 自动选择最佳时钟，同时 SLEEP=0 */
#define ICM20608_USER_I2C_IF_DIS	0x10	/* 关闭 I2C 接口，只用 SPI */
#define ICM20608_GYRO_FS_2000DPS	(3 << 3)	/* 16.4 LSB/(deg/s) */
#define ICM20608_ACCEL_FS_2G		(0 << 3)	/* 16384 LSB/g */
#define ICM20608_DLPF_CFG		3	/* 陀螺仪带宽约 41 Hz */
#define ICM20608_A_DLPF_CFG		3	/* 加速度计带宽约 44.8 Hz */

struct icm20608_chip {
	const char *name;
	u8 whoami;
};

static const struct icm20608_chip icm20608g = { "ICM-20608-G", 0xAF };
static const struct icm20608_chip icm20608d = { "ICM-20608-D", 0xAE };

static const struct of_device_id icm20608_of_match[] = {
	{ .compatible = "invensense,icm20608",  .data = &icm20608g },
	{ .compatible = "invensense,icm20608d", .data = &icm20608d },
	{ }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static int icm20608_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
	u8 tx = reg | 0x80;

	return spi_write_then_read(spi, &tx, 1, val, 1);
}

/* 连续读 len 个寄存器：芯片会自动把地址加 1 */
static int icm20608_read_regs(struct spi_device *spi, u8 reg, u8 *buf, size_t len)
{
	u8 tx = reg | 0x80;

	return spi_write_then_read(spi, &tx, 1, buf, len);
}

static int icm20608_write_reg(struct spi_device *spi, u8 reg, u8 val)
{
	u8 tx[2] = { reg & 0x7F, val };	/* 最高位 0 = 写 */

	/* 不直接用 spi_write：它要求缓冲区能做 DMA，栈上的数组不行；
	 * spi_write_then_read 会先拷贝到内部缓冲区，小数据量这样最省心 */
	return spi_write_then_read(spi, tx, sizeof(tx), NULL, 0);
}

static int icm20608_chip_init(struct spi_device *spi)
{
	static const struct {
		u8 reg;
		u8 val;
	} cfg[] = {
		{ ICM20608_REG_USER_CTRL,     ICM20608_USER_I2C_IF_DIS },
		{ ICM20608_REG_PWR_MGMT_2,    0x00 },	/* 6 个轴全开 */
		{ ICM20608_REG_SMPLRT_DIV,    0x00 },	/* 1 kHz / (1 + 0) */
		{ ICM20608_REG_CONFIG,        ICM20608_DLPF_CFG },
		{ ICM20608_REG_GYRO_CONFIG,   ICM20608_GYRO_FS_2000DPS },
		{ ICM20608_REG_ACCEL_CONFIG,  ICM20608_ACCEL_FS_2G },
		{ ICM20608_REG_ACCEL_CONFIG2, ICM20608_A_DLPF_CFG },
	};
	u8 val;
	int i, ret;

	/* 1. 软件复位，手册：复位后最长 100 ms 才能读写寄存器 */
	ret = icm20608_write_reg(spi, ICM20608_REG_PWR_MGMT_1,
				 ICM20608_PWR1_DEVICE_RESET);
	if (ret)
		return ret;
	msleep(100);

	/* 2. 先唤醒：复位后芯片处于睡眠，睡眠时部分寄存器（如 USER_CTRL）
	 *    写不进去。手册：从睡眠到可读写最长 5 ms */
	ret = icm20608_write_reg(spi, ICM20608_REG_PWR_MGMT_1,
				 ICM20608_PWR1_CLKSEL_AUTO);
	if (ret)
		return ret;
	msleep(10);

	/* 3. 逐个写配置，并读回校验 */
	for (i = 0; i < ARRAY_SIZE(cfg); i++) {
		ret = icm20608_write_reg(spi, cfg[i].reg, cfg[i].val);
		if (ret)
			return ret;
		ret = icm20608_read_reg(spi, cfg[i].reg, &val);
		if (ret)
			return ret;
		if (val != cfg[i].val) {
			dev_err(&spi->dev, "reg 0x%02x: wrote 0x%02x, read 0x%02x\n",
				cfg[i].reg, cfg[i].val, val);
			return -EIO;
		}
	}

	/* 4. 陀螺仪从睡眠启动需要约 35 ms 才稳定 */
	msleep(50);
	return 0;
}

/* 读一次六轴 + 温度并打印（v1 第 2 小步的临时验证代码） */
static void icm20608_dump_sample(struct spi_device *spi)
{
	u8 buf[14];
	s16 ax, ay, az, t, gx, gy, gz;

	if (icm20608_read_regs(spi, ICM20608_REG_ACCEL_XOUT_H, buf, sizeof(buf)))
		return;

	/* 每个量 2 字节，高字节在前（大端） */
	ax = (s16)((buf[0] << 8) | buf[1]);
	ay = (s16)((buf[2] << 8) | buf[3]);
	az = (s16)((buf[4] << 8) | buf[5]);
	t  = (s16)((buf[6] << 8) | buf[7]);
	gx = (s16)((buf[8] << 8) | buf[9]);
	gy = (s16)((buf[10] << 8) | buf[11]);
	gz = (s16)((buf[12] << 8) | buf[13]);

	/* 内核里不用浮点：换算成 mg、0.01 摄氏度、0.1 度/秒 */
	dev_info(&spi->dev, "accel raw %6d %6d %6d -> %5d %5d %5d mg\n",
		 ax, ay, az, ax * 1000 / 16384, ay * 1000 / 16384, az * 1000 / 16384);
	dev_info(&spi->dev, "gyro  raw %6d %6d %6d -> %5d %5d %5d x0.1 dps\n",
		 gx, gy, gz, gx * 100 / 164, gy * 100 / 164, gz * 100 / 164);
	dev_info(&spi->dev, "temp  raw %6d -> %d.%02d C\n",
		 t, (t * 1000 / 3268 + 2500) / 100, (t * 1000 / 3268 + 2500) % 100);
}

static int icm20608_probe(struct spi_device *spi)
{
	const struct of_device_id *match;
	const struct icm20608_chip *chip;
	u8 id;
	int ret;

	match = of_match_device(icm20608_of_match, &spi->dev);
	if (!match)
		return -ENODEV;
	chip = match->data;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	ret = icm20608_read_reg(spi, ICM20608_REG_WHO_AM_I, &id);
	if (ret) {
		dev_err(&spi->dev, "read WHO_AM_I failed: %d\n", ret);
		return ret;
	}

	if (id != chip->whoami) {
		dev_err(&spi->dev, "WHO_AM_I = 0x%02x, expect 0x%02x for %s\n",
			id, chip->whoami, chip->name);
		return -ENODEV;
	}

	dev_info(&spi->dev, "%s detected, WHO_AM_I = 0x%02x\n", chip->name, id);

	ret = icm20608_chip_init(spi);
	if (ret) {
		dev_err(&spi->dev, "chip init failed: %d\n", ret);
		return ret;
	}
	dev_info(&spi->dev, "chip initialized: accel +-2g, gyro +-2000dps, 1 kHz\n");

	icm20608_dump_sample(spi);
	return 0;
}

static int icm20608_remove(struct spi_device *spi)
{
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
MODULE_DESCRIPTION("ICM-20608 6-axis IMU SPI driver");
