/*
 * Unix 域套接字服务端：一行一条消息（以 '\n' 结尾）
 *
 * - 监听 fd 与客户端 fd 都是非阻塞的，挂在主循环的 epoll 上（data.ptr 指向 server 或 client）
 * - 每个客户端一个输入缓冲（按行切分后回调 on_command）和一个输出队列：
 *   send 写不完的部分留在队列里并打开 EPOLLOUT，可写时继续发
 * - 慢客户端保护：输出队列超过 OUT_MAX 就断开它，不让一个卡住的界面拖住采集
 * - 断开采用延迟回收：先标记 dead，等当前回调链结束再 close/free，
 *   避免在处理某个客户端的命令时（命令里可能广播）把它自己释放掉
 */
#include "devmond.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CLIENTS	8
#define IN_MAX		1024
#define OUT_MAX		(256 * 1024)

struct client {
	int fd;
	bool dead;
	bool want_out;
	char in[IN_MAX];
	size_t inlen;
	char *out;
	size_t outlen;
	size_t outcap;
	struct client *next;
};

struct server {
	int fd;
	int epfd;
	int busy;		/* >0 时正在回调中，推迟回收 */
	char path[108];
	struct client *clients;
	int nclients;
	command_cb on_command;
	connect_cb on_connect;
	void *ctx;
};

static void client_update_events(struct server *srv, struct client *c)
{
	struct epoll_event ev = {
		.events = EPOLLIN | EPOLLRDHUP | (c->outlen ? EPOLLOUT : 0),
		.data.ptr = c,
	};
	bool want = c->outlen > 0;

	if (want != c->want_out && !epoll_ctl(srv->epfd, EPOLL_CTL_MOD, c->fd, &ev))
		c->want_out = want;
}

static void client_flush(struct server *srv, struct client *c)
{
	while (c->outlen && !c->dead) {
		ssize_t n = send(c->fd, c->out, c->outlen, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				c->dead = true;
			break;
		}
		memmove(c->out, c->out + n, c->outlen - n);
		c->outlen -= n;
	}
	if (!c->dead)
		client_update_events(srv, c);
}

static void client_vsend(struct server *srv, struct client *c, const char *fmt, va_list ap)
{
	char line[512];
	int n;

	if (c->dead)
		return;
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;	/* 超长消息截断，仍保证以换行结束 */
	line[n++] = '\n';

	if (c->outlen + n > OUT_MAX) {
		dlog("client %d too slow (%zu bytes queued), disconnecting", c->fd, c->outlen);
		c->dead = true;
		return;
	}
	if (c->outlen + n > c->outcap) {
		size_t cap = c->outcap ? c->outcap * 2 : 4096;
		char *p;

		while (cap < c->outlen + n)
			cap *= 2;
		p = realloc(c->out, cap);
		if (!p) {
			c->dead = true;
			return;
		}
		c->out = p;
		c->outcap = cap;
	}
	memcpy(c->out + c->outlen, line, n);
	c->outlen += n;
	client_flush(srv, c);
}

void client_send(struct server *srv, struct client *c, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	client_vsend(srv, c, fmt, ap);
	va_end(ap);
}

static void reap(struct server *srv)
{
	struct client **pp = &srv->clients;

	if (srv->busy)
		return;
	while (*pp) {
		struct client *c = *pp;

		if (!c->dead) {
			pp = &c->next;
			continue;
		}
		*pp = c->next;
		epoll_ctl(srv->epfd, EPOLL_CTL_DEL, c->fd, NULL);
		close(c->fd);
		free(c->out);
		free(c);
		srv->nclients--;
		dlog("client disconnected (%d left)", srv->nclients);
	}
}

void server_broadcast(struct server *srv, const char *fmt, ...)
{
	struct client *c;

	for (c = srv->clients; c; c = c->next) {
		va_list ap;

		va_start(ap, fmt);
		client_vsend(srv, c, fmt, ap);
		va_end(ap);
	}
	reap(srv);
}

int server_client_count(const struct server *srv)
{
	return srv->nclients;
}

static void accept_clients(struct server *srv)
{
	for (;;) {
		struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP };
		struct client *c;
		int fd = accept4(srv->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (fd < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				dlog("accept: %s", strerror(errno));
			return;
		}
		if (srv->nclients >= MAX_CLIENTS || !(c = calloc(1, sizeof(*c)))) {
			static const char busy[] = "ERR CONNECT msg=too many clients\n";

			send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
			close(fd);
			continue;
		}
		c->fd = fd;
		ev.data.ptr = c;
		if (epoll_ctl(srv->epfd, EPOLL_CTL_ADD, fd, &ev)) {
			close(fd);
			free(c);
			continue;
		}
		c->next = srv->clients;
		srv->clients = c;
		srv->nclients++;
		dlog("client connected (%d total)", srv->nclients);
		if (srv->on_connect)
			srv->on_connect(srv, c, srv->ctx);
	}
}

static void client_read(struct server *srv, struct client *c)
{
	for (;;) {
		ssize_t n = read(c->fd, c->in + c->inlen, IN_MAX - c->inlen);
		char *start, *nl;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				c->dead = true;
			return;
		}
		if (n == 0) {
			c->dead = true;
			return;
		}
		c->inlen += n;

		/* 按行切分；命令回调可能往任何客户端发消息，但不会释放客户端（busy） */
		start = c->in;
		while (!c->dead && (nl = memchr(start, '\n', c->in + c->inlen - start))) {
			*nl = '\0';
			if (nl > start && nl[-1] == '\r')
				nl[-1] = '\0';
			if (*start)
				srv->on_command(srv, c, start, srv->ctx);
			start = nl + 1;
		}
		c->inlen -= start - c->in;
		memmove(c->in, start, c->inlen);
		if (c->inlen == IN_MAX) {
			dlog("client %d sent an over-long line, disconnecting", c->fd);
			c->dead = true;
			return;
		}
	}
}

bool server_dispatch(struct server *srv, void *ptr, uint32_t events)
{
	struct client *c;

	if (ptr == srv) {
		accept_clients(srv);
		return true;
	}
	for (c = srv->clients; c && c != ptr; c = c->next)
		;
	if (!c)
		return false;

	srv->busy++;
	if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
		client_read(srv, c);
	if (events & EPOLLOUT)
		client_flush(srv, c);
	if (events & (EPOLLHUP | EPOLLERR))
		c->dead = true;
	srv->busy--;
	reap(srv);
	return true;
}

int server_open(struct server **out, int epfd, const char *path,
		command_cb on_command, connect_cb on_connect, void *ctx)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct epoll_event ev = { .events = EPOLLIN };
	struct server *srv;

	if (strlen(path) >= sizeof(addr.sun_path)) {
		dlog("socket path too long: %s", path);
		return -1;
	}
	srv = calloc(1, sizeof(*srv));
	if (!srv)
		return -1;
	srv->epfd = epfd;
	srv->on_command = on_command;
	srv->on_connect = on_connect;
	srv->ctx = ctx;
	snprintf(srv->path, sizeof(srv->path), "%s", path);
	strcpy(addr.sun_path, path);

	srv->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (srv->fd < 0) {
		dlog("socket: %s", strerror(errno));
		free(srv);
		return -1;
	}
	/* 调用方已用 pid 文件锁保证只有一个实例，这里删除上次异常退出遗留的套接字文件 */
	unlink(path);
	if (bind(srv->fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(srv->fd, 4)) {
		dlog("bind/listen %s: %s", path, strerror(errno));
		close(srv->fd);
		free(srv);
		return -1;
	}
	ev.data.ptr = srv;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, srv->fd, &ev)) {
		dlog("epoll add listen fd: %s", strerror(errno));
		close(srv->fd);
		unlink(path);
		free(srv);
		return -1;
	}
	*out = srv;
	return 0;
}

void server_close(struct server *srv)
{
	struct client *c;

	if (!srv)
		return;
	for (c = srv->clients; c; c = c->next)
		c->dead = true;
	srv->busy = 0;
	reap(srv);
	epoll_ctl(srv->epfd, EPOLL_CTL_DEL, srv->fd, NULL);
	close(srv->fd);
	unlink(srv->path);
	free(srv);
}
