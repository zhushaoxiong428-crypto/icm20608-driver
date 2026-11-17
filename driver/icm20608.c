#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/of_device.h>

#define ICM20608_REG_WHO_AM_I	0x75

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
