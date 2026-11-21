/* LED（/dev/led0，写 '0'/'1'）与 input 按键 */
#include "devmond.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int led_open(const char *path)
{
	return open(path, O_WRONLY | O_CLOEXEC);
}

int led_set(int fd, bool on)
{
	char value = on ? '1' : '0';
	ssize_t n;

	do {
		n = write(fd, &value, 1);
	} while (n < 0 && errno == EINTR);
	return n == 1 ? 0 : (n < 0 ? -errno : -EIO);
}

int input_open_by_name(const char *name, char *path, size_t len)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *e;
	int fd = -1;

	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL && fd < 0) {
		char attr[300], buf[64];

		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(attr, sizeof(attr), "/sys/class/input/%s/device", e->d_name);
		if (iio_read_attr(attr, "name", buf, sizeof(buf)) || strcmp(buf, name))
			continue;
		snprintf(path, len, "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	closedir(d);
	return fd;
}
