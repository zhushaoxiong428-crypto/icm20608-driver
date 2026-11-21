/*
 * devmonctl：devmond 的命令行客户端
 *
 *   devmonctl STATUS                 打印当前状态
 *   devmonctl LED on|off|toggle      其他命令见 protocol.md，只打印应答行（-v 打印全部）
 *   devmonctl watch [秒数] [类型...]   打印推送的消息，例如 watch 10 AP PROX
 */
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "devmond.h"

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool type_selected(const char *line, char **types, int ntypes)
{
	size_t len = strcspn(line, " ");
	int i;

	if (ntypes == 0)
		return true;
	for (i = 0; i < ntypes; i++)
		if (strlen(types[i]) == len && !strncmp(line, types[i], len))
			return true;
	return false;
}

/* 是否为 cmd 的应答：OK <CMD> / ERR <CMD> ...，PING 的应答是 PONG */
static bool is_reply(const char *line, const char *cmd)
{
	size_t len = strlen(cmd);

	if (!strcmp(cmd, "PING"))
		return !strcmp(line, "PONG");
	if (!strncmp(line, "OK ", 3))
		return !strncmp(line + 3, cmd, len) && (line[3 + len] == '\0' || line[3 + len] == ' ');
	if (!strncmp(line, "ERR ", 4))
		return !strncmp(line + 4, cmd, len) && line[4 + len] == ' ';
	return false;
}

int main(int argc, char *argv[])
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	const char *path = DEVMOND_DEFAULT_SOCKET;
	bool verbose = false, watch;
	char buf[4096], cmd[64] = "";
	size_t len = 0;
	double deadline;
	int fd, opt, i, status = 1;

	while ((opt = getopt(argc, argv, "s:vh")) != -1) {
		if (opt == 's')
			path = optarg;
		else if (opt == 'v')
			verbose = true;
		else
			goto usage;
	}
	if (optind >= argc)
		goto usage;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		fprintf(stderr, "connect %s: %s (is devmond running?)\n", path, strerror(errno));
		return 1;
	}

	watch = !strcmp(argv[optind], "watch");
	if (watch) {
		double secs = optind + 1 < argc ? atof(argv[optind + 1]) : 0;

		deadline = secs > 0 ? now_s() + secs : 0;
		optind += secs > 0 ? 2 : 1;
	} else {
		char line[512];
		int n = 0;

		for (i = optind; i < argc; i++)
			n += snprintf(line + n, sizeof(line) - n, "%s%s", i > optind ? " " : "", argv[i]);
		snprintf(cmd, sizeof(cmd), "%.*s", (int)strcspn(line, " "), line);
		line[n++] = '\n';
		if (write(fd, line, n) != n) {
			perror("write");
			return 1;
		}
		deadline = now_s() + 3;	/* 应答超时 */
	}

	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int timeout = deadline > 0 ? (int)((deadline - now_s()) * 1000) : -1;
		ssize_t n;
		char *start, *nl;

		if (deadline > 0 && timeout <= 0)
			break;
		if (poll(&pfd, 1, timeout) <= 0)
			continue;
		n = read(fd, buf + len, sizeof(buf) - 1 - len);
		if (n <= 0) {
			fprintf(stderr, "devmond closed the connection\n");
			break;
		}
		len += n;
		start = buf;
		while ((nl = memchr(start, '\n', buf + len - start))) {
			*nl = '\0';
			if (watch) {
				if (type_selected(start, argv + optind, argc - optind)) {
					printf("%s\n", start);
					fflush(stdout);
				}
			} else if (is_reply(start, cmd)) {
				printf("%s\n", start);
				status = strncmp(start, "ERR", 3) ? 0 : 1;
				goto done;
			} else if (verbose) {
				printf("%s\n", start);
			}
			start = nl + 1;
		}
		len -= start - buf;
		memmove(buf, start, len);
	}
	if (watch)
		status = 0;
	else
		fprintf(stderr, "no reply to %s\n", cmd);
done:
	close(fd);
	return status;

usage:
	fprintf(stderr,
		"usage: %s [-s socket] [-v] COMMAND [ARGS]\n"
		"       %s [-s socket] watch [seconds] [TYPE...]\n"
		"commands: PING STATUS LED on|off|toggle AUTO on|off READ REC start|stop\n"
		"          CONFIG on=N off=N interval=MS\n",
		argv[0], argv[0]);
	return 2;
}
