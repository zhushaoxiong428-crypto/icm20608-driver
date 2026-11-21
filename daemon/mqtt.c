/*
 * MQTT 连接与协议桥接
 *
 * 状态机：IDLE（等待重试）-> CONNECTING（非阻塞 TCP 握手）-> WAIT_CONNACK -> CONNECTED
 *   任何错误都回到 IDLE，按 1、2、4 … 30 s 退避重试；连接成功后退避清零。
 * 连接成功后：
 *   - 发布 <prefix>/status = "online"（retained）；CONNECT 里的遗嘱是 "offline"（retained），
 *     断线、断电、进程被杀时由 broker 代发
 *   - 订阅 <prefix>/cmd（QoS 1）：消息内容就是 devmond 的命令行（"LED on"、"REC start"），
 *     经 server_command() 执行，应答发到 <prefix>/reply
 *   - 在 server 上挂一个虚拟客户端：每条广播（AP、IMU、SYS、LED、PROX …）转成
 *     <prefix>/<类型小写>，内容为 JSON；状态类（led、config、rec）retained，新订阅者立即拿到当前值
 * 发送全部 QoS 0：遥测本来就周期性刷新，丢一条无妨；发送缓冲满（broker 跟不上）时丢弃并计数，
 * 绝不阻塞主循环。保活：超过 keepalive 没收到 broker 的任何报文就发 PINGREQ，
 * 再过 keepalive 还没有回应就断开重连（网线拔掉时，只靠 TCP 要很久才发现）。
 */
#include "devmond.h"
#include "mqtt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define MQTT_DEFAULT_PORT	1883
#define KEEPALIVE_S		30
#define CONNECT_TIMEOUT_MS	10000
#define BACKOFF_MAX_S		30
#define OUT_MAX			(32 * 1024)
#define IN_MAX			4096	/* broker 发来的报文（命令）不会很长 */
#define PKT_MAX			1600	/* 一条协议行（最长 512）转成 JSON 再加主题 */
#define DROP_LOG_MS		60000

/* 状态类消息 retained：broker 保存最后一条，订阅时立即收到 */
static const char *const kRetained[] = { "led", "config", "rec", NULL };

enum mqtt_state { MS_IDLE, MS_CONNECTING, MS_WAIT_CONNACK, MS_CONNECTED };

struct mqtt {
	int epfd;
	struct server *srv;
	struct client *client;	/* CONNECTED 期间挂在 server 上的虚拟客户端 */
	struct sockaddr_in addr;
	char broker[48];	/* "IP:端口"，日志和状态用 */
	char prefix[96];
	char client_id[64];
	char topic_cmd[128];
	char topic_status[128];
	char topic_reply[128];

	int fd;
	int timer_fd;
	char w_sock, w_timer;	/* 只用地址：epoll data.ptr 指向它们，dispatch 据此认出自己的 fd */
	bool sock_in_epoll;

	enum mqtt_state state;
	int64_t state_since;	/* 以下时间均为 CLOCK_MONOTONIC 毫秒 */
	int64_t retry_at;
	int backoff_s;
	int64_t last_rx;
	int64_t ping_sent;
	bool ping_outstanding;
	bool broken;		/* sink 里发现写失败：不能在广播途中断开，推迟到下一次 dispatch */
	char last_err[96];

	uint8_t out[OUT_MAX];
	size_t outlen;
	bool want_out;
	uint8_t in[IN_MAX];
	size_t inlen;

	unsigned long dropped;
	int64_t dropped_logged;
};

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void mqtt_status_line(const struct mqtt *m, char *buf, size_t len)
{
	static const char *const names[] = { "retry", "connecting", "connecting", "connected" };

	if (!m) {
		snprintf(buf, len, "MQTT state=off");
	} else if (m->state == MS_IDLE) {
		int64_t left = m->retry_at - now_ms();

		snprintf(buf, len, "MQTT state=retry broker=%s topic=%s retry=%lld msg=%s",
			 m->broker, m->prefix, (long long)(left > 0 ? (left + 999) / 1000 : 0),
			 m->last_err);
	} else {
		snprintf(buf, len, "MQTT state=%s broker=%s topic=%s", names[m->state],
			 m->broker, m->prefix);
	}
}

/* 状态变化广播给所有客户端（界面显示）；虚拟客户端的 sink 会忽略 MQTT 类型 */
static void set_state(struct mqtt *m, enum mqtt_state st)
{
	char line[256];

	m->state = st;
	m->state_since = now_ms();
	mqtt_status_line(m, line, sizeof(line));
	server_broadcast(m->srv, "%s", line);
}

static void update_events(struct mqtt *m)
{
	struct epoll_event ev = { .data.ptr = &m->w_sock };
	bool want = m->state == MS_CONNECTING || m->outlen > 0;

	if (m->fd < 0 || (m->sock_in_epoll && want == m->want_out))
		return;
	ev.events = EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0);
	if (!epoll_ctl(m->epfd, m->sock_in_epoll ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, m->fd, &ev)) {
		m->sock_in_epoll = true;
		m->want_out = want;
	}
}

static void flush(struct mqtt *m)
{
	while (m->outlen && !m->broken) {
		ssize_t n = send(m->fd, m->out, m->outlen, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				snprintf(m->last_err, sizeof(m->last_err), "send: %s", strerror(errno));
				m->broken = true;
			}
			break;
		}
		memmove(m->out, m->out + n, m->outlen - n);
		m->outlen -= n;
	}
	update_events(m);
}

/*
 * 排队一个报文。droppable：发送缓冲放不下时丢弃（遥测）；否则（控制报文）放不下
 * 说明连接已经堵死，标记断开
 */
static void queue(struct mqtt *m, const uint8_t *pkt, size_t n, bool droppable)
{
	if (n == 0) {
		/* build 函数的 buf 不够大：编程错误，记下来，不要悄悄什么都不发 */
		dlog("mqtt: packet build failed (buffer too small?)");
		return;
	}
	if (m->fd < 0 || m->broken)
		return;
	if (m->outlen + n > sizeof(m->out)) {
		if (droppable) {
			m->dropped++;
			return;
		}
		snprintf(m->last_err, sizeof(m->last_err), "send buffer full");
		m->broken = true;
		return;
	}
	memcpy(m->out + m->outlen, pkt, n);
	m->outlen += n;
	flush(m);
}

static void publish(struct mqtt *m, const char *topic, const char *payload, bool retain)
{
	uint8_t pkt[PKT_MAX];
	size_t n = mqtt_build_publish(pkt, sizeof(pkt), topic, payload, strlen(payload), retain);

	if (n)
		queue(m, pkt, n, true);
}

static bool is_retained(const char *type)
{
	int i;

	for (i = 0; kRetained[i]; i++)
		if (!strcmp(type, kRetained[i]))
			return true;
	return false;
}

/* 虚拟客户端收到的每一行：协议行 -> MQTT 消息（在 server 的广播循环里调用，只能排队） */
static void sink(const char *line, void *ctx)
{
	struct mqtt *m = ctx;
	char type[32], json[PKT_MAX - 200], topic[sizeof(m->prefix) + sizeof(type) + 1];

	if (m->state != MS_CONNECTED)
		return;
	if (!strncmp(line, "HELLO", 5) || !strncmp(line, "MQTT", 4))
		return;
	/* 命令应答和错误原样发到 reply，便于对照命令看 */
	if (!strncmp(line, "OK ", 3) || !strncmp(line, "ERR ", 4) || !strcmp(line, "PONG")) {
		publish(m, m->topic_reply, line, false);
		return;
	}
	if (mqtt_line_to_json(line, realtime_ns() / 1000000, type, sizeof(type), json, sizeof(json))) {
		dlog("mqtt: cannot convert \"%.60s\"", line);
		return;
	}
	snprintf(topic, sizeof(topic), "%s/%s", m->prefix, type);
	publish(m, topic, json, is_retained(type));
}

static void close_socket(struct mqtt *m)
{
	if (m->fd >= 0) {
		if (m->sock_in_epoll)
			epoll_ctl(m->epfd, EPOLL_CTL_DEL, m->fd, NULL);
		close(m->fd);
	}
	m->fd = -1;
	m->sock_in_epoll = false;
	m->want_out = false;
	m->outlen = 0;
	m->inlen = 0;
	m->broken = false;
	m->ping_outstanding = false;
}

static void fail(struct mqtt *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void fail(struct mqtt *m, const char *fmt, ...)
{
	va_list ap;

	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(m->last_err, sizeof(m->last_err), fmt, ap);
		va_end(ap);
	}
	if (m->client) {
		server_detach(m->srv, m->client);
		m->client = NULL;
	}
	close_socket(m);
	m->retry_at = now_ms() + m->backoff_s * 1000;
	dlog("mqtt: %s, retry in %d s", m->last_err, m->backoff_s);
	m->backoff_s = m->backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : m->backoff_s * 2;
	set_state(m, MS_IDLE);
}

static void send_connect(struct mqtt *m)
{
	uint8_t pkt[512];
	size_t n = mqtt_build_connect(pkt, sizeof(pkt), m->client_id, KEEPALIVE_S,
				      m->topic_status, "offline");

	m->last_rx = now_ms();
	set_state(m, MS_WAIT_CONNACK);
	queue(m, pkt, n, false);
}

static void start_connect(struct mqtt *m)
{
	int on = 1;

	m->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (m->fd < 0) {
		fail(m, "socket: %s", strerror(errno));
		return;
	}
	setsockopt(m->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	if (connect(m->fd, (struct sockaddr *)&m->addr, sizeof(m->addr)) == 0) {
		update_events(m);
		send_connect(m);
	} else if (errno == EINPROGRESS) {
		set_state(m, MS_CONNECTING);
		update_events(m);	/* 握手完成时可写 */
	} else {
		fail(m, "connect: %s", strerror(errno));
	}
}

static void on_connack(struct mqtt *m, const uint8_t *body, uint32_t len)
{
	static const char *const reasons[] = {
		"accepted", "unacceptable protocol version", "client id rejected",
		"server unavailable", "bad user name or password", "not authorized",
	};
	uint8_t pkt[256];
	size_t n;

	if (m->state != MS_WAIT_CONNACK || len != 2) {
		fail(m, "unexpected CONNACK");
		return;
	}
	if (body[1]) {
		fail(m, "connection refused: %s",
		     body[1] < sizeof(reasons) / sizeof(reasons[0]) ? reasons[body[1]] : "unknown");
		return;
	}
	m->backoff_s = 1;
	set_state(m, MS_CONNECTED);
	dlog("mqtt: connected to %s as %s, topics %s/#", m->broker, m->client_id, m->prefix);

	publish(m, m->topic_status, "online", true);
	n = mqtt_build_subscribe(pkt, sizeof(pkt), 1, m->topic_cmd, 1);
	queue(m, pkt, n, false);
	/* 挂上虚拟客户端：on_connect 把完整状态发一遍，retained 主题随之刷新 */
	m->client = server_attach(m->srv, sink, m);
	if (!m->client)
		fail(m, "out of memory");
}

static void on_publish(struct mqtt *m, uint8_t flags, const uint8_t *body, uint32_t len)
{
	struct mqtt_publish p;
	char line[512];
	size_t n;

	if (mqtt_parse_publish(flags, body, len, &p)) {
		fail(m, "malformed PUBLISH");
		return;
	}
	if (p.topic_len == strlen(m->topic_cmd) && !memcmp(p.topic, m->topic_cmd, p.topic_len)) {
		if (p.retain) {
			/* 订阅时 broker 补发的保留消息：每次重连都会再来一遍，执行它是错的 */
			dlog("mqtt: ignoring retained message on %s", m->topic_cmd);
		} else if (m->client) {
			n = p.payload_len < sizeof(line) - 1 ? p.payload_len : sizeof(line) - 1;
			memcpy(line, p.payload, n);
			line[n] = '\0';
			line[strcspn(line, "\r\n")] = '\0';	/* 只取第一行 */
			if (*line)
				server_command(m->srv, m->client, line);
		}
	}
	if (p.qos == 1) {
		uint8_t pkt[8];

		n = mqtt_build_puback(pkt, sizeof(pkt), p.packet_id);
		queue(m, pkt, n, false);
	}
}

static void handle_packet(struct mqtt *m, uint8_t first, const uint8_t *body, uint32_t len)
{
	m->last_rx = now_ms();
	m->ping_outstanding = false;	/* broker 有任何回应都说明连接是活的 */
	switch (first >> 4) {
	case MQTT_CONNACK:
		on_connack(m, body, len);
		break;
	case MQTT_PUBLISH:
		on_publish(m, first & 0x0f, body, len);
		break;
	case MQTT_SUBACK:
		if (len == 3 && body[2] == 0x80)
			dlog("mqtt: subscription to %s refused", m->topic_cmd);
		break;
	case MQTT_PINGRESP:
		break;
	default:
		dlog("mqtt: ignoring packet type %d", first >> 4);
		break;
	}
}

static void on_readable(struct mqtt *m)
{
	for (;;) {
		ssize_t n = recv(m->fd, m->in + m->inlen, sizeof(m->in) - m->inlen, MSG_DONTWAIT);
		size_t pos = 0;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				fail(m, "recv: %s", strerror(errno));
			return;
		}
		if (n == 0) {
			fail(m, "broker closed the connection");
			return;
		}
		m->inlen += n;

		/* 按报文切分：固定报头 1 字节 + 剩余长度 + 内容 */
		while (m->fd >= 0) {
			uint32_t len;
			size_t used;
			int r = m->inlen - pos >= 2 ?
				mqtt_decode_len(m->in + pos + 1, m->inlen - pos - 1, &len, &used) : 1;

			if (r < 0) {
				fail(m, "malformed packet");
				return;
			}
			if (r > 0)
				break;
			if (1 + used + len > sizeof(m->in)) {
				fail(m, "packet too large (%u bytes)", len);
				return;
			}
			if (m->inlen - pos < 1 + used + len)
				break;
			handle_packet(m, m->in[pos], m->in + pos + 1 + used, len);
			pos += 1 + used + len;
		}
		if (m->fd < 0)
			return;	/* handle_packet 里断开了 */
		memmove(m->in, m->in + pos, m->inlen - pos);
		m->inlen -= pos;
	}
}

static void on_socket(struct mqtt *m, uint32_t events)
{
	if (m->state == MS_CONNECTING) {
		int err = 0;
		socklen_t len = sizeof(err);

		if (!(events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
			return;
		getsockopt(m->fd, SOL_SOCKET, SO_ERROR, &err, &len);
		if (err) {
			fail(m, "connect: %s", strerror(err));
			return;
		}
		send_connect(m);
		return;
	}
	if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
		on_readable(m);
	if (m->fd >= 0 && (events & EPOLLOUT))
		flush(m);
}

static void on_timer(struct mqtt *m)
{
	uint64_t expirations;
	int64_t now = now_ms();

	if (read(m->timer_fd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
		dlog("mqtt: read timerfd: %s", strerror(errno));

	if (m->dropped && now - m->dropped_logged >= DROP_LOG_MS) {
		dlog("mqtt: dropped %lu messages, broker not keeping up", m->dropped);
		m->dropped = 0;
		m->dropped_logged = now;
	}

	switch (m->state) {
	case MS_IDLE:
		if (now >= m->retry_at)
			start_connect(m);
		break;
	case MS_CONNECTING:
	case MS_WAIT_CONNACK:
		if (now - m->state_since > CONNECT_TIMEOUT_MS)
			fail(m, "%s timed out", m->state == MS_CONNECTING ? "connect" : "CONNACK");
		break;
	case MS_CONNECTED:
		if (m->ping_outstanding && now - m->ping_sent > KEEPALIVE_S * 1000) {
			fail(m, "no response from broker for %d s", 2 * KEEPALIVE_S);
		} else if (!m->ping_outstanding && now - m->last_rx >= KEEPALIVE_S * 1000) {
			uint8_t pkt[8];	/* 报文 2 字节，build 还要预留 5 字节报头 */

			queue(m, pkt, mqtt_build_pingreq(pkt, sizeof(pkt)), false);
			m->ping_outstanding = true;
			m->ping_sent = now;
		}
		break;
	}
}

bool mqtt_dispatch(struct mqtt *m, void *ptr, uint32_t events)
{
	if (ptr == &m->w_sock && m->fd >= 0)
		on_socket(m, events);
	else if (ptr == &m->w_timer)
		on_timer(m);
	else
		return ptr == &m->w_sock;	/* 套接字刚关闭时残留的事件，同样是自己的 */
	if (m->broken)
		fail(m, NULL);	/* last_err 已经写好 */
	return true;
}

int mqtt_open(struct mqtt **out, int epfd, struct server *srv, const char *broker,
	      const char *prefix, const char *client_id)
{
	struct itimerspec its = { .it_interval = { 1, 0 }, .it_value = { 1, 0 } };
	struct epoll_event ev = { .events = EPOLLIN };
	char host[48];
	const char *colon = strrchr(broker, ':');
	int port = MQTT_DEFAULT_PORT;
	struct mqtt *m;
	size_t hl = colon ? (size_t)(colon - broker) : strlen(broker);

	if (hl == 0 || hl >= sizeof(host)) {
		dlog("mqtt: bad broker address \"%s\"", broker);
		return -1;
	}
	memcpy(host, broker, hl);
	host[hl] = '\0';
	if (colon && ((port = atoi(colon + 1)) <= 0 || port > 65535)) {
		dlog("mqtt: bad port in \"%s\"", broker);
		return -1;
	}
	m = calloc(1, sizeof(*m));
	if (!m)
		return -1;
	m->addr.sin_family = AF_INET;
	m->addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &m->addr.sin_addr) != 1) {
		dlog("mqtt: \"%s\" is not an IPv4 address (host names are not resolved)", host);
		free(m);
		return -1;
	}
	m->epfd = epfd;
	m->srv = srv;
	m->fd = -1;
	m->backoff_s = 1;
	snprintf(m->broker, sizeof(m->broker), "%s:%d", host, port);
	snprintf(m->prefix, sizeof(m->prefix), "%s", prefix);
	snprintf(m->client_id, sizeof(m->client_id), "%s", client_id);
	snprintf(m->topic_cmd, sizeof(m->topic_cmd), "%s/cmd", prefix);
	snprintf(m->topic_status, sizeof(m->topic_status), "%s/status", prefix);
	snprintf(m->topic_reply, sizeof(m->topic_reply), "%s/reply", prefix);
	snprintf(m->last_err, sizeof(m->last_err), "not connected yet");

	m->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	ev.data.ptr = &m->w_timer;
	if (m->timer_fd < 0 || timerfd_settime(m->timer_fd, 0, &its, NULL)
	    || epoll_ctl(epfd, EPOLL_CTL_ADD, m->timer_fd, &ev)) {
		dlog("mqtt: timer: %s", strerror(errno));
		if (m->timer_fd >= 0)
			close(m->timer_fd);
		free(m);
		return -1;
	}
	*out = m;
	dlog("mqtt: broker %s, client id %s, topics %s/#", m->broker, client_id, prefix);
	start_connect(m);
	return 0;
}

void mqtt_close(struct mqtt *m)
{
	if (!m)
		return;
	if (m->client)
		server_detach(m->srv, m->client);
	close_socket(m);
	epoll_ctl(m->epfd, EPOLL_CTL_DEL, m->timer_fd, NULL);
	close(m->timer_fd);
	free(m);
}
