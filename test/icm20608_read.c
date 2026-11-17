/*
 * 读 /dev/icm20608 并换算成物理单位打印。
 * 用法：./icm20608_read [次数] [间隔毫秒]，默认 10 次、间隔 200 ms
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

#include "../driver/icm20608_uapi.h"

int main(int argc, char *argv[])
{
	int n = argc > 1 ? atoi(argv[1]) : 10;
	int interval_ms = argc > 2 ? atoi(argv[2]) : 200;
	struct icm20608_sample s;
	int fd, i;

	fd = open("/dev/icm20608", O_RDONLY);
	if (fd < 0) {
		perror("open /dev/icm20608");
		return 1;
	}

	printf("%8s %8s %8s %6s | %8s %8s %8s | %6s\n",
	       "ax[g]", "ay[g]", "az[g]", "|a|", "gx[dps]", "gy[dps]", "gz[dps]", "T[C]");

	for (i = 0; i < n; i++) {
		ssize_t r = read(fd, &s, sizeof(s));	/* -> icm20608_read() */

		if (r != (ssize_t)sizeof(s)) {
			perror("read");
			close(fd);
			return 1;
		}

		double ax = (double)s.accel[0] / ICM20608_ACCEL_LSB_PER_G;
		double ay = (double)s.accel[1] / ICM20608_ACCEL_LSB_PER_G;
		double az = (double)s.accel[2] / ICM20608_ACCEL_LSB_PER_G;
		double gx = s.gyro[0] * 10.0 / ICM20608_GYRO_LSB_PER_DPS_X10;
		double gy = s.gyro[1] * 10.0 / ICM20608_GYRO_LSB_PER_DPS_X10;
		double gz = s.gyro[2] * 10.0 / ICM20608_GYRO_LSB_PER_DPS_X10;
		double t = s.temp / 326.8 + 25.0;

		printf("%8.3f %8.3f %8.3f %6.3f | %8.2f %8.2f %8.2f | %6.2f\n",
		       ax, ay, az, sqrt(ax * ax + ay * ay + az * az),
		       gx, gy, gz, t);
		usleep(interval_ms * 1000);
	}

	close(fd);
	return 0;
}
