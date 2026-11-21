/*
 * Unix 域套接字服务端：一行一条消息（以 '\n' 结尾）
 *
 * - 监听 fd 与客户端 fd 都是非阻塞的，挂在主循环的 epoll 上（data.ptr 指向 server 或 client）
 * - 每个客户端一个输入缓冲（按行切分后回调 on_command）和一个输出队列：
 *   send 写不完的部分留在队列里并打开 EPOLLOUT，可写时继续发
 * - 慢客户端保护：输出队列超过 OUT_MAX 就断开它，不让一个卡住的界面拖住采集
 * - 断开采用延迟回收：先标记 dead，等当前回调链结束再 close/free，
 *   避免在处理某个客户端的命令时（命令里可能广播）把它自己释放掉
 * - 可同时监听 Unix 套接字（本机界面）和 TCP（远程访问，协议相同）；
 *   TCP 连接打开 TCP_NODELAY（命令应答不等 Nagle）和 keepalive（网线拔掉后约 1 分钟发现对端已失联）
 * - 虚拟客户端（server_attach）：没有 fd，发给它的每一行（广播和命令应答）直接回调 sink；
 *   MQTT 桥接（mqtt.c）就是这样一个客户端，把协议行转成 MQTT 消息，收到的命令经 server_command 执行
 */
#include "devmond.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CLIENTS	8
#define IN_MAX		1024
#define OUT_MAX		(256 * 1024)

struct client {
	int fd;			/* 虚拟客户端为 -1 */
	line_sink sink;		/* 非 NULL 表示虚拟客户端 */
	void *sink_ctx;
	char peer[48];		/* "local" 或 TCP 对端 IP:端口，用于日志 */
	bool dead;
	bool want_out;
	char in[IN_MAX];
	size_t inlen;
	char *out;
	size_t outlen;
	size_t outcap;
	struct client *next;
};

struct listener {
	int fd;
	bool tcp;
};

struct server {
	struct listener listeners[2];	/* [0] Unix 套接字，[1] 可选 TCP */
	int nlisteners;
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
	if (c->sink) {
		c->sink(line, c->sink_ctx);	/* 截断的长行同样交给 sink，不加换行 */
		return;
	}
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
		if (c->sink) {
			free(c);	/* 虚拟客户端不占连接数，也没有 fd */
			continue;
		}
		epoll_ctl(srv->epfd, EPOLL_CTL_DEL, c->fd, NULL);
		close(c->fd);
		dlog("client %s disconnected (%d left)", c->peer, srv->nclients - 1);
		free(c->out);
		free(c);
		srv->nclients--;
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

struct client *server_attach(struct server *srv, line_sink sink, void *ctx)
{
	struct client *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	c->fd = -1;
	c->sink = sink;
	c->sink_ctx = ctx;
	c->next = srv->clients;
	srv->clients = c;
	/* 和真实客户端一样，连上就收到一遍完整状态 */
	if (srv->on_connect) {
		srv->busy++;
		srv->on_connect(srv, c, srv->ctx);
		srv->busy--;
	}
	return c;
}

void server_detach(struct server *srv, struct client *c)
{
	c->dead = true;
	reap(srv);
}

void server_command(struct server *srv, struct client *c, char *line)
{
	srv->busy++;	/* 命令里的广播可能让别的客户端出错，回调结束后再回收 */
	srv->on_command(srv, c, line, srv->ctx);
	srv->busy--;
	reap(srv);
}

static void tune_tcp(int fd)
{
	int on = 1, idle = 30, interval = 10, count = 3;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

static void accept_clients(struct server *srv, struct listener *l)
{
	for (;;) {
		struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP };
		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		struct client *c;
		int fd = accept4(l->fd, l->tcp ? (struct sockaddr *)&peer : NULL,
				 l->tcp ? &peer_len : NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

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
		if (l->tcp) {
			char ip[INET_ADDRSTRLEN] = "?";

			inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
			snprintf(c->peer, sizeof(c->peer), "%s:%u", ip, ntohs(peer.sin_port));
			tune_tcp(fd);
		} else {
			snprintf(c->peer, sizeof(c->peer), "local");
		}
		ev.data.ptr = c;
		if (epoll_ctl(srv->epfd, EPOLL_CTL_ADD, fd, &ev)) {
			close(fd);
			free(c);
			continue;
		}
		c->next = srv->clients;
		srv->clients = c;
		srv->nclients++;
		dlog("client %s connected (%d total)", c->peer, srv->nclients);
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
	int i;

	for (i = 0; i < srv->nlisteners; i++) {
		if (ptr == &srv->listeners[i]) {
			accept_clients(srv, &srv->listeners[i]);
			return true;
		}
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

	srv->listeners[0].fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (srv->listeners[0].fd < 0) {
		dlog("socket: %s", strerror(errno));
		free(srv);
		return -1;
	}
	/* 调用方已用 pid 文件锁保证只有一个实例，这里删除上次异常退出遗留的套接字文件 */
	unlink(path);
	if (bind(srv->listeners[0].fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(srv->listeners[0].fd, 4)) {
		dlog("bind/listen %s: %s", path, strerror(errno));
		close(srv->listeners[0].fd);
		free(srv);
		return -1;
	}
	ev.data.ptr = &srv->listeners[0];
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, srv->listeners[0].fd, &ev)) {
		dlog("epoll add listen fd: %s", strerror(errno));
		close(srv->listeners[0].fd);
		unlink(path);
		free(srv);
		return -1;
	}
	srv->nlisteners = 1;
	*out = srv;
	return 0;
}

/* 远程访问：同一协议再监听一个 TCP 端口（局域网用，无认证） */
int server_listen_tcp(struct server *srv, int port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	struct listener *l = &srv->listeners[srv->nlisteners];
	struct epoll_event ev = { .events = EPOLLIN, .data.ptr = l };
	int on = 1;

	if (srv->nlisteners >= (int)(sizeof(srv->listeners) / sizeof(srv->listeners[0])))
		return -1;
	l->tcp = true;
	l->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (l->fd < 0) {
		dlog("tcp socket: %s", strerror(errno));
		return -1;
	}
	/* 重启时不必等 TIME_WAIT 结束 */
	setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	if (bind(l->fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(l->fd, 4)
	    || epoll_ctl(srv->epfd, EPOLL_CTL_ADD, l->fd, &ev)) {
		dlog("tcp port %d: %s", port, strerror(errno));
		close(l->fd);
		return -1;
	}
	srv->nlisteners++;
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
	{
		int i;

		for (i = 0; i < srv->nlisteners; i++) {
			epoll_ctl(srv->epfd, EPOLL_CTL_DEL, srv->listeners[i].fd, NULL);
			close(srv->listeners[i].fd);
		}
	}
	unlink(srv->path);
	free(srv);
}
