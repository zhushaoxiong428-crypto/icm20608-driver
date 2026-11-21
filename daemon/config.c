/*
 * device_monitor.ini（与原 Qt 版 QSettings 写出的格式相同）：
 *   [led]    onThreshold=20  offThreshold=50
 *   [sensor] intervalMs=1000 imuRateHz=100
 * 保存时先写临时文件、fsync，再 rename 替换：断电时要么是旧文件，要么是完整的新文件
 */
#include "devmond.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void config_defaults(struct config *c)
{
	c->on_threshold = 20;
	c->off_threshold = 50;
	c->interval_ms = 1000;
	c->imu_rate_hz = 100;
}

bool config_valid(const struct config *c, char *err)
{
	if (c->on_threshold < 0 || c->off_threshold > 65535 || c->on_threshold >= c->off_threshold) {
		snprintf(err, ERRLEN, "LED ON threshold must be lower than LED OFF threshold (0~65535)");
		return false;
	}
	if (c->interval_ms < 200 || c->interval_ms > 60000) {
		snprintf(err, ERRLEN, "sample interval must be 200~60000 ms");
		return false;
	}
	if (c->imu_rate_hz < 4 || c->imu_rate_hz > 1000) {
		snprintf(err, ERRLEN, "imuRateHz must be 4~1000");
		return false;
	}
	return true;
}

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';
	return s;
}

static bool parse_int(const char *s, int *out)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (end == s || *end)
		return false;
	*out = (int)v;
	return true;
}

int config_load(const char *path, struct config *c)
{
	char line[256], section[32] = "";
	struct config tmp;
	bool ok = true;
	FILE *f;

	config_defaults(c);
	f = fopen(path, "r");
	if (!f)
		return 1;

	tmp = *c;
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line), *eq;
		int *target = NULL;

		if (!*s || *s == ';' || *s == '#')
			continue;
		if (*s == '[') {
			snprintf(section, sizeof(section), "%.*s", (int)strcspn(s + 1, "]"), s + 1);
			continue;
		}
		eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = '\0';
		s = trim(s);

		if (!strcmp(section, "led") && !strcmp(s, "onThreshold"))
			target = &tmp.on_threshold;
		else if (!strcmp(section, "led") && !strcmp(s, "offThreshold"))
			target = &tmp.off_threshold;
		else if (!strcmp(section, "sensor") && !strcmp(s, "intervalMs"))
			target = &tmp.interval_ms;
		else if (!strcmp(section, "sensor") && !strcmp(s, "imuRateHz"))
			target = &tmp.imu_rate_hz;
		if (target && !parse_int(trim(eq + 1), target))
			ok = false;
	}
	fclose(f);

	{
		char err[ERRLEN];

		if (!ok || !config_valid(&tmp, err))
			return -1;
	}
	*c = tmp;
	return 0;
}

int config_save(const char *path, const struct config *c, char *err)
{
	char tmp[300];
	FILE *f;
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0 || !(f = fdopen(fd, "w"))) {
		snprintf(err, ERRLEN, "open %s: %s", tmp, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}

	fprintf(f, "[led]\nonThreshold=%d\noffThreshold=%d\n\n"
		   "[sensor]\nintervalMs=%d\nimuRateHz=%d\n",
		c->on_threshold, c->off_threshold, c->interval_ms, c->imu_rate_hz);

	if (fflush(f) || fsync(fd)) {
		snprintf(err, ERRLEN, "write %s: %s", tmp, strerror(errno));
		fclose(f);
		unlink(tmp);
		return -1;
	}
	fclose(f);

	if (rename(tmp, path)) {
		snprintf(err, ERRLEN, "rename to %s: %s", path, strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}
