#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "icm20608_uapi.h"

#define ICM20608_DEV_NAME	"icm20608"

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
#define ICM20608_PWR1_SLEEP		0x40	/* 进入睡眠，省电 */
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

/*
 * 每个设备一份的状态。v1 先只支持一个设备（板子上只有一颗 IMU），
 * class 和设备号在 probe 里申请；v2 改用 IIO 子系统后这部分会被替换。
 */
struct icm20608_dev {
	struct spi_device *spi;
	const struct icm20608_chip *chip;
	dev_t devt;		/* 设备号 */
	struct cdev cdev;	/* 字符设备对象，挂着 file_operations */
	struct class *class;	/* /sys/class/icm20608 */
	struct device *device;	/* 让 mdev/udev 创建 /dev/icm20608 */
};

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

/* 读一次六轴 + 温度：14 字节，每个量高字节在前（大端），转成 CPU 字节序 */
static int icm20608_read_sample(struct spi_device *spi,
				struct icm20608_sample *s)
{
	u8 buf[14];
	int i, ret;

	ret = icm20608_read_regs(spi, ICM20608_REG_ACCEL_XOUT_H, buf, sizeof(buf));
	if (ret)
		return ret;

	for (i = 0; i < 3; i++) {
		s->accel[i] = (s16)((buf[2 * i] << 8) | buf[2 * i + 1]);
		s->gyro[i]  = (s16)((buf[8 + 2 * i] << 8) | buf[9 + 2 * i]);
	}
	s->temp = (s16)((buf[6] << 8) | buf[7]);
	return 0;
}

/* ---------- file_operations：/dev/icm20608 的系统调用入口 ---------- */

static int icm20608_open(struct inode *inode, struct file *filp)
{
	/* 由 cdev 找回整个设备结构体，存起来给 read 用 */
	filp->private_data = container_of(inode->i_cdev, struct icm20608_dev, cdev);
	return 0;
}

static int icm20608_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * read：每次调用都从芯片读一份最新数据。传感器是"数据流"，没有文件
 * 位置的概念，所以忽略 *ppos；用户缓冲区小于一个样本时返回 -EINVAL。
 */
static ssize_t icm20608_read(struct file *filp, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct icm20608_dev *idev = filp->private_data;
	struct icm20608_sample s;
	int ret;

	if (count < sizeof(s))
		return -EINVAL;

	ret = icm20608_read_sample(idev->spi, &s);
	if (ret)
		return ret;

	if (copy_to_user(ubuf, &s, sizeof(s)))
		return -EFAULT;

	return sizeof(s);
}

static const struct file_operations icm20608_fops = {
	.owner   = THIS_MODULE,
	.open    = icm20608_open,
	.release = icm20608_release,
	.read    = icm20608_read,
	.llseek  = no_llseek,
};

/* 注册字符设备：设备号 -> cdev -> class -> device（/dev 节点） */
static int icm20608_cdev_register(struct icm20608_dev *idev)
{
	int ret;

	ret = alloc_chrdev_region(&idev->devt, 0, 1, ICM20608_DEV_NAME);
	if (ret)
		return ret;

	cdev_init(&idev->cdev, &icm20608_fops);
	idev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&idev->cdev, idev->devt, 1);
	if (ret)
		goto err_region;

	idev->class = class_create(THIS_MODULE, ICM20608_DEV_NAME);
	if (IS_ERR(idev->class)) {
		ret = PTR_ERR(idev->class);
		goto err_cdev;
	}

	idev->device = device_create(idev->class, &idev->spi->dev, idev->devt,
				     NULL, ICM20608_DEV_NAME);
	if (IS_ERR(idev->device)) {
		ret = PTR_ERR(idev->device);
		goto err_class;
	}
	return 0;

	/* 出错时按申请的相反顺序释放 */
err_class:
	class_destroy(idev->class);
err_cdev:
	cdev_del(&idev->cdev);
err_region:
	unregister_chrdev_region(idev->devt, 1);
	return ret;
}

static void icm20608_cdev_unregister(struct icm20608_dev *idev)
{
	device_destroy(idev->class, idev->devt);
	class_destroy(idev->class);
	cdev_del(&idev->cdev);
	unregister_chrdev_region(idev->devt, 1);
}

static int icm20608_probe(struct spi_device *spi)
{
	const struct of_device_id *match;
	const struct icm20608_chip *chip;
	struct icm20608_dev *idev;
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
	/* devm_：设备解绑时内核自动释放，remove 里不用手动 kfree */
	idev = devm_kzalloc(&spi->dev, sizeof(*idev), GFP_KERNEL);
	if (!idev)
		return -ENOMEM;
	idev->spi = spi;
	idev->chip = chip;
	spi_set_drvdata(spi, idev);	/* remove 时用 spi_get_drvdata 取回 */

	ret = icm20608_cdev_register(idev);
	if (ret) {
		dev_err(&spi->dev, "register char device failed: %d\n", ret);
		return ret;
	}

	dev_info(&spi->dev, "ready: /dev/%s (major %d), accel +-2g, gyro +-2000dps\n",
		 ICM20608_DEV_NAME, MAJOR(idev->devt));
	return 0;
}

static int icm20608_remove(struct spi_device *spi)
{
	struct icm20608_dev *idev = spi_get_drvdata(spi);

	icm20608_cdev_unregister(idev);
	/* 让芯片回到睡眠，省电 */
	icm20608_write_reg(spi, ICM20608_REG_PWR_MGMT_1, ICM20608_PWR1_SLEEP);
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
