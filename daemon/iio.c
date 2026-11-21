#include "devmond.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IIO_ROOT "/sys/bus/iio/devices"

/* 按 name 文件查找 IIO 设备（names 以 NULL 结尾，任一匹配即可），iio:deviceN 编号不能写死 */
int iio_find_device(const char *const *names, char *dir, size_t dirlen,
		    char *node, size_t nodelen)
{
	DIR *d = opendir(IIO_ROOT);
	struct dirent *e;
	int found = -ENODEV;

	if (!d)
		return -errno;

	while ((e = readdir(d)) != NULL && found) {
		char path[300], name[64];
		const char *const *n;

		if (strncmp(e->d_name, "iio:device", 10))
			continue;
		snprintf(path, sizeof(path), "%s/%s", IIO_ROOT, e->d_name);
		if (iio_read_attr(path, "name", name, sizeof(name)))
			continue;
		for (n = names; *n; n++) {
			if (!strcmp(name, *n)) {
				snprintf(dir, dirlen, "%s", path);
				snprintf(node, nodelen, "/dev/%s", e->d_name);
				found = 0;
				break;
			}
		}
	}
	closedir(d);
	return found;
}

int iio_write_attr(const char *dir, const char *attr, const char *value)
{
	char path[300];
	size_t len = strlen(value);
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	/* sysfs 写一次 write 下发，驱动返回的 EINVAL/EBUSY 在这里拿到 */
	n = write(fd, value, len);
	if (n < 0)
		n = -errno;
	close(fd);
	return n == (ssize_t)len ? 0 : (n < 0 ? (int)n : -EIO);
}

int iio_read_attr(const char *dir, const char *attr, char *buf, size_t len)
{
	char path[300];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	if (n < 0)
		n = -errno;
	close(fd);
	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return 0;
}

int iio_read_double(const char *dir, const char *attr, double *out)
{
	char buf[64], *end;
	int ret = iio_read_attr(dir, attr, buf, sizeof(buf));

	if (ret)
		return ret;
	*out = strtod(buf, &end);
	return end == buf ? -EINVAL : 0;
}
