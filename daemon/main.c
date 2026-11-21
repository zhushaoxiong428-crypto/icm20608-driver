/*
 * devmond 主循环
 *
 * 一个线程、一个 epoll，所有输入都是 fd：
 *   signalfd   SIGINT/SIGTERM 正常退出；SIGHUP 重新读取配置文件
 *   timerfd    采集周期（interval）：读 AP3216C、自动亮灯、发布 IMU 平均值、刷新 CSV
 *   timerfd    100 ms：读走 ICM20608 IIO 缓冲区，样本累计统计并逐个录制
 *   AP 事件 fd  AP_INT 中断产生的接近事件
 *   按键 fd     input 事件，手动模式下翻转 LED
 *   套接字      客户端命令与数据推送（server.c）
 * 没有共享数据、不需要锁；任何一个 fd 都不会阻塞住其他 fd。
 */
#include "devmond.h"
#include "ap3216c_data.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define IMU_DRAINS_PER_SEC	10
#define LED_DEVICE		"/dev/led0"
#define KEY_INPUT_NAME		"keyinput"

static const double kStandardGravity = 9.80665;
static const double kRadToDeg = 57.29577951308232;

enum watch_kind { W_SIGNAL, W_TICK, W_DRAIN, W_KEY, W_AP_EVENT };

struct watch {
	enum watch_kind kind;
};

struct app {
	int epfd;
	bool running;
	struct server *srv;

	struct config cfg;
	char config_path[PATH_MAX];
	char records_dir[PATH_MAX];

	struct watch w_signal, w_tick, w_drain, w_key, w_ap_event;
	int signal_fd, tick_fd, drain_fd;

	struct imu imu;
	bool imu_ok;
	char imu_err[ERRLEN];
	int imu_n;			/* 本发布周期的样本数与累加值 */
	double imu_sum[7];		/* ax ay az gx gy gz temp */
	int64_t imu_first_ts, imu_last_ts;

	struct ap ap;
	bool ap_ok;
	char ap_err[ERRLEN];
	int prox_state;			/* -1 未知，0 远，1 近 */
	int prox_count;

	int led_fd;
	bool led_on;
	bool auto_mode;

	int key_fd;
	const char *key_state;

	struct recorder rec;
	struct sysmon sysmon;
};

/* ---------------- 状态消息 ---------------- */

/* c 为 NULL 时广播给所有客户端，否则只发给 c */
static void emit(struct app *a, struct client *c, const char *line)
{
	if (c)
		client_send(a->srv, c, "%s", line);
	else
		server_broadcast(a->srv, "%s", line);
}

static void send_config(struct app *a, struct client *c)
{
	char line[400];

	snprintf(line, sizeof(line), "CONFIG on=%d off=%d interval=%d imu_rate=%d path=%s",
		 a->cfg.on_threshold, a->cfg.off_threshold, a->cfg.interval_ms,
		 a->cfg.imu_rate_hz, a->config_path);
	emit(a, c, line);
}

static const char *led_state(const struct app *a)
{
	return a->led_fd < 0 ? "unavailable" : (a->led_on ? "on" : "off");
}

static void send_led(struct app *a, struct client *c)
{
	char line[64];

	snprintf(line, sizeof(line), "LED state=%s auto=%d", led_state(a), a->auto_mode);
	emit(a, c, line);
}

static void send_rec(struct app *a, struct client *c)
{
	char line[400];

	snprintf(line, sizeof(line), "REC state=%s dir=%s", a->rec.on ? "on" : "off",
		 a->records_dir);
	emit(a, c, line);
}

/* 新客户端连上时把完整状态发一遍，界面无需关心自己是何时连上的 */
static void on_connect(struct server *srv, struct client *c, void *ctx)
{
	struct app *a = ctx;

	client_send(srv, c, "HELLO devmond proto=%d", DEVMOND_PROTO_VERSION);
	send_config(a, c);
	send_led(a, c);
	client_send(srv, c, "KEY state=%s", a->key_state);
	send_rec(a, c);
	if (a->prox_state >= 0)
		client_send(srv, c, "PROX state=%s count=%d", a->prox_state ? "near" : "far",
			    a->prox_count);
	if (!a->ap_ok)
		client_send(srv, c, "AP_ERR msg=%s", a->ap_err);
	if (!a->imu_ok)
		client_send(srv, c, "IMU_ERR msg=%s", a->imu_err);
}

/* ---------------- LED 与自动控制 ---------------- */

static int set_led(struct app *a, bool on, char *err)
{
	int ret;

	if (a->led_fd < 0) {
		snprintf(err, ERRLEN, "LED unavailable");
		return -1;
	}
	ret = led_set(a->led_fd, on);
	if (ret) {
		snprintf(err, ERRLEN, "write LED: %s", strerror(-ret));
		return -1;
	}
	if (a->led_on != on) {
		a->led_on = on;
		send_led(a, NULL);
	}
	return 0;
}

/* 双阈值滞回：<= on 开灯，>= off 关灯，中间保持，避免在阈值附近来回闪 */
static void auto_led(struct app *a, int als)
{
	bool target = a->led_on;
	char err[ERRLEN];

	if (als <= a->cfg.on_threshold)
		target = true;
	else if (als >= a->cfg.off_threshold)
		target = false;
	if (target != a->led_on && set_led(a, target, err))
		server_broadcast(a->srv, "ERR AUTO msg=%s", err);
}

/* ---------------- 传感器 ---------------- */

static void read_ap(struct app *a)
{
	int ir, als, ps, flags;
	char err[ERRLEN];

	if (!a->ap_ok)
		return;
	if (ap_read(&a->ap, &ir, &als, &ps, &flags, err)) {
		server_broadcast(a->srv, "AP_ERR msg=%s", err);
		return;
	}

	server_broadcast(a->srv, "AP ir=%d als=%d lux=%.2f ps=%d flags=%d", ir, als,
			 als * a->ap.lux_per_count, ps, flags);
	recorder_ap(&a->rec, realtime_ns(), ir, als, als * a->ap.lux_per_count, ps, flags);
	if (a->auto_mode && (flags & AP3216C_ALS_VALID))
		auto_led(a, als);
}

static void on_imu_sample(const struct imu_sample *s, void *ctx)
{
	struct app *a = ctx;
	int i;

	for (i = 0; i < 3; i++) {
		a->imu_sum[i] += s->accel[i];
		a->imu_sum[3 + i] += s->gyro[i];
	}
	a->imu_sum[6] += s->temp_c;
	if (a->imu_n++ == 0)
		a->imu_first_ts = s->ts_ns;
	a->imu_last_ts = s->ts_ns;
	recorder_imu(&a->rec, s);
}

static void stop_recording(struct app *a, const char *error)
{
	if (!a->rec.on)
		return;
	recorder_stop(&a->rec);
	if (error) {
		dlog("recording stopped: %s", error);
		server_broadcast(a->srv, "REC state=error msg=%s", error);
	} else {
		send_rec(a, NULL);
	}
}

static void drain_imu(struct app *a)
{
	char err[ERRLEN];

	if (!a->imu_ok)
		return;
	if (imu_drain(&a->imu, on_imu_sample, a, err) < 0) {
		dlog("%s", err);
		snprintf(a->imu_err, sizeof(a->imu_err), "%s", err);
		a->imu_ok = false;
		imu_close(&a->imu);
		server_broadcast(a->srv, "IMU_ERR msg=%s", err);
	}
}

/* 一个周期内全部样本的平均值，比单点读数更稳；采样率按内核时间戳算 */
static void publish_imu(struct app *a)
{
	double n = a->imu_n, rate = 0;
	int i;

	if (!a->imu_ok)
		return;
	drain_imu(a);	/* 把最后不到 100 ms 的样本也算进本周期 */
	if (!a->imu_ok)
		return;
	if (a->imu_n == 0) {
		server_broadcast(a->srv, "IMU_ERR msg=no ICM20608 samples in the last interval");
		return;
	}
	n = a->imu_n;
	if (a->imu_n > 1 && a->imu_last_ts > a->imu_first_ts)
		rate = (a->imu_n - 1) * 1e9 / (double)(a->imu_last_ts - a->imu_first_ts);

	server_broadcast(a->srv,
			 "IMU n=%d rate=%.1f ax=%.3f ay=%.3f az=%.3f gx=%.2f gy=%.2f gz=%.2f temp=%.2f",
			 a->imu_n, rate, a->imu_sum[0] / n / kStandardGravity,
			 a->imu_sum[1] / n / kStandardGravity, a->imu_sum[2] / n / kStandardGravity,
			 a->imu_sum[3] / n * kRadToDeg, a->imu_sum[4] / n * kRadToDeg,
			 a->imu_sum[5] / n * kRadToDeg, a->imu_sum[6] / n);

	a->imu_n = 0;
	for (i = 0; i < 7; i++)
		a->imu_sum[i] = 0;
}

static void on_proximity(bool near, int64_t ts_ns, void *ctx)
{
	struct app *a = ctx;

	a->prox_state = near;
	a->prox_count++;
	server_broadcast(a->srv, "PROX state=%s count=%d ts=%lld", near ? "near" : "far",
			 a->prox_count, (long long)ts_ns);
}

/* ---------------- 定时器 ---------------- */

static int timer_set(int fd, int interval_ms)
{
	struct itimerspec its = {
		.it_interval = { interval_ms / 1000, (interval_ms % 1000) * 1000000L },
		.it_value = { interval_ms / 1000, (interval_ms % 1000) * 1000000L },
	};

	return timerfd_settime(fd, 0, &its, NULL);
}

static void timer_ack(int fd)
{
	uint64_t expirations;

	if (read(fd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
		dlog("read timerfd: %s", strerror(errno));
}

static void publish_sys(struct app *a)
{
	struct sysmon_sample s;

	sysmon_sample(&a->sysmon, a->records_dir, &s);
	server_broadcast(a->srv,
			 "SYS cpu=%.1f self_cpu=%.2f mem_used=%.1f mem_total=%.1f load=%.2f "
			 "uptime=%ld temp=%.1f rx=%.1f tx=%.1f disk_free=%.0f disk_total=%.0f",
			 s.cpu_pct, s.self_cpu_pct, s.mem_used_mb, s.mem_total_mb, s.load1,
			 s.uptime_s, s.temp_ok ? s.temp_c : -273.0, s.rx_kbps, s.tx_kbps,
			 s.disk_ok ? s.disk_free_mb : -1.0, s.disk_ok ? s.disk_total_mb : -1.0);
}

static void on_tick(struct app *a)
{
	char err[ERRLEN];

	timer_ack(a->tick_fd);
	read_ap(a);
	publish_imu(a);
	publish_sys(a);
	if (recorder_flush(&a->rec, err))
		stop_recording(a, err);
}

/* ---------------- 按键与事件 fd ---------------- */

static void on_key(struct app *a)
{
	struct input_event ev[16];

	for (;;) {
		ssize_t n = read(a->key_fd, ev, sizeof(ev));
		ssize_t i;

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
				dlog("key input lost");
				epoll_ctl(a->epfd, EPOLL_CTL_DEL, a->key_fd, NULL);
				close(a->key_fd);
				a->key_fd = -1;
				a->key_state = "unavailable";
				server_broadcast(a->srv, "KEY state=unavailable");
			}
			return;
		}
		for (i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
			char err[ERRLEN];

			if (ev[i].type != EV_KEY || ev[i].code != KEY_ENTER || ev[i].value > 1)
				continue;	/* 只关心按下（1）和松开（0），忽略自动重复（2） */
			a->key_state = ev[i].value ? "pressed" : "released";
			server_broadcast(a->srv, "KEY state=%s", a->key_state);
			/* 手动模式下按键翻转 LED；自动模式只显示按键状态 */
			if (ev[i].value == 1 && !a->auto_mode && a->led_fd >= 0
			    && set_led(a, !a->led_on, err))
				server_broadcast(a->srv, "ERR KEY msg=%s", err);
		}
	}
}

static void on_ap_event(struct app *a)
{
	char err[ERRLEN];

	if (ap_read_events(&a->ap, on_proximity, a, err) < 0) {
		dlog("%s", err);
		epoll_ctl(a->epfd, EPOLL_CTL_DEL, a->ap.event_fd, NULL);
		server_broadcast(a->srv, "AP_ERR msg=%s", err);
	}
}

/* ---------------- 命令 ---------------- */

/* 从 "on=20 off=50" 里取 key 对应的整数，没有该 key 返回 1 */
static int get_int_field(const char *args, const char *key, int *out)
{
	size_t klen = strlen(key);
	const char *p = args;

	while ((p = strstr(p, key)) != NULL) {
		if ((p == args || p[-1] == ' ') && p[klen] == '=') {
			char *end;
			long v = strtol(p + klen + 1, &end, 10);

			if (end == p + klen + 1 || (*end && *end != ' '))
				return -1;
			*out = (int)v;
			return 0;
		}
		p += klen;
	}
	return 1;
}

static void cmd_config(struct app *a, struct client *c, const char *args)
{
	struct config next = a->cfg;
	char err[ERRLEN];

	if (get_int_field(args, "on", &next.on_threshold) < 0
	    || get_int_field(args, "off", &next.off_threshold) < 0
	    || get_int_field(args, "interval", &next.interval_ms) < 0) {
		client_send(a->srv, c, "ERR CONFIG msg=bad number");
		return;
	}
	if (!config_valid(&next, err)) {
		client_send(a->srv, c, "ERR CONFIG msg=%s", err);
		return;
	}

	if (next.interval_ms != a->cfg.interval_ms)
		timer_set(a->tick_fd, next.interval_ms);
	a->cfg = next;
	send_config(a, NULL);

	if (config_save(a->config_path, &a->cfg, err)) {
		dlog("%s", err);
		client_send(a->srv, c, "ERR CONFIG msg=applied for this run, but saving failed: %s",
			    err);
		return;
	}
	client_send(a->srv, c, "OK CONFIG");
}

static void on_command(struct server *srv, struct client *c, char *line, void *ctx)
{
	struct app *a = ctx;
	char *args = strchr(line, ' ');
	char err[ERRLEN];

	if (args)
		*args++ = '\0';
	else
		args = line + strlen(line);

	if (!strcmp(line, "PING")) {
		client_send(srv, c, "PONG");
	} else if (!strcmp(line, "STATUS")) {
		on_connect(srv, c, a);
		client_send(srv, c, "OK STATUS");
	} else if (!strcmp(line, "LED")) {
		bool on = !strcmp(args, "on") ? true : !strcmp(args, "off") ? false : !a->led_on;

		if (strcmp(args, "on") && strcmp(args, "off") && strcmp(args, "toggle"))
			client_send(srv, c, "ERR LED msg=usage: LED on|off|toggle");
		else if (a->auto_mode)
			client_send(srv, c, "ERR LED msg=auto mode is on");
		else if (set_led(a, on, err))
			client_send(srv, c, "ERR LED msg=%s", err);
		else
			client_send(srv, c, "OK LED");
	} else if (!strcmp(line, "AUTO")) {
		if (strcmp(args, "on") && strcmp(args, "off")) {
			client_send(srv, c, "ERR AUTO msg=usage: AUTO on|off");
		} else if (a->led_fd < 0) {
			client_send(srv, c, "ERR AUTO msg=LED unavailable");
		} else {
			a->auto_mode = !strcmp(args, "on");
			send_led(a, NULL);
			client_send(srv, c, "OK AUTO");
		}
	} else if (!strcmp(line, "CONFIG")) {
		cmd_config(a, c, args);
	} else if (!strcmp(line, "READ")) {
		read_ap(a);
		client_send(srv, c, "OK READ");
	} else if (!strcmp(line, "REC")) {
		if (!strcmp(args, "start")) {
			if (recorder_start(&a->rec, a->records_dir, err)) {
				client_send(srv, c, "ERR REC msg=%s", err);
			} else {
				send_rec(a, NULL);
				client_send(srv, c, "OK REC");
			}
		} else if (!strcmp(args, "stop")) {
			stop_recording(a, NULL);
			client_send(srv, c, "OK REC");
		} else {
			client_send(srv, c, "ERR REC msg=usage: REC start|stop");
		}
	} else {
		client_send(srv, c, "ERR %s msg=unknown command", line);
	}
}

/* ---------------- 启动、守护进程化、单实例 ---------------- */

static int epoll_add(struct app *a, int fd, struct watch *w, enum watch_kind kind)
{
	struct epoll_event ev = { .events = EPOLLIN, .data.ptr = w };

	w->kind = kind;
	return epoll_ctl(a->epfd, EPOLL_CTL_ADD, fd, &ev);
}

/* 两次 fork：脱离终端和会话、不会重新获得控制终端；标准输出输入改到日志文件 */
static int daemonize(const char *log_path)
{
	pid_t pid = fork();
	int fd;

	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);
	if (setsid() < 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		_exit(0);

	umask(022);
	if (chdir("/"))
		return -1;
	fd = open("/dev/null", O_RDWR);
	if (fd >= 0) {
		dup2(fd, STDIN_FILENO);
		close(fd);
	}
	fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		close(fd);
	}
	return 0;
}

/* flock 加锁的 pid 文件：进程退出（包括崩溃）时内核自动释放锁，不会留下“僵尸锁” */
static int lock_pidfile(const char *path)
{
	char buf[32];
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);

	if (fd < 0) {
		dlog("open pid file %s: %s", path, strerror(errno));
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB)) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);

		buf[n > 0 ? n : 0] = '\0';
		dlog("already running (pid %s)", n > 0 ? strtok(buf, "\n") : "?");
		close(fd);
		return -1;
	}
	if (ftruncate(fd, 0) == 0) {
		int n = snprintf(buf, sizeof(buf), "%d\n", getpid());

		if (write(fd, buf, n) != n)
			dlog("write pid file: %s", strerror(errno));
	}
	return fd;	/* 保持打开以持有锁 */
}

static void exe_dir(char *out, size_t len)
{
	char path[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);

	if (n <= 0) {
		snprintf(out, len, ".");
		return;
	}
	path[n] = '\0';
	snprintf(out, len, "%s", dirname(path));
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d] [-l log] [-s socket] [-c config.ini] [-r records_dir] [-p pidfile]\n"
		"  -d  run as a daemon (log to -l, default /tmp/devmond.log)\n"
		"  defaults: socket %s, pidfile %s, config and records next to the executable\n",
		prog, DEVMOND_DEFAULT_SOCKET, DEVMOND_DEFAULT_PIDFILE);
}

int main(int argc, char *argv[])
{
	static struct app app;
	struct app *a = &app;
	const char *socket_path = DEVMOND_DEFAULT_SOCKET;
	const char *pid_path = DEVMOND_DEFAULT_PIDFILE;
	const char *log_path = "/tmp/devmond.log";
	char dir[PATH_MAX], key_path[64], err[ERRLEN];
	bool daemon_mode = false;
	sigset_t mask;
	int opt, pid_fd, ret;

	exe_dir(dir, sizeof(dir));
	snprintf(a->config_path, sizeof(a->config_path), "%s/device_monitor.ini", dir);
	snprintf(a->records_dir, sizeof(a->records_dir), "%s/records", dir);

	while ((opt = getopt(argc, argv, "dl:s:c:r:p:h")) != -1) {
		switch (opt) {
		case 'd': daemon_mode = true; break;
		case 'l': log_path = optarg; break;
		case 's': socket_path = optarg; break;
		case 'c': snprintf(a->config_path, sizeof(a->config_path), "%s", optarg); break;
		case 'r': snprintf(a->records_dir, sizeof(a->records_dir), "%s", optarg); break;
		case 'p': pid_path = optarg; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 2;
		}
	}

	if (daemon_mode && daemonize(log_path)) {
		perror("daemonize");
		return 1;
	}
	pid_fd = lock_pidfile(pid_path);
	if (pid_fd < 0)
		return 1;

	/* 信号改由 signalfd 在主循环里同步处理，没有异步信号处理函数的重入问题 */
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	signal(SIGPIPE, SIG_IGN);

	a->epfd = epoll_create1(EPOLL_CLOEXEC);
	a->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	a->tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	a->drain_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (a->epfd < 0 || a->signal_fd < 0 || a->tick_fd < 0 || a->drain_fd < 0) {
		dlog("create epoll/signalfd/timerfd: %s", strerror(errno));
		return 1;
	}

	ret = config_load(a->config_path, &a->cfg);
	dlog("config %s: %s", a->config_path,
	     ret == 0 ? "loaded" : ret > 0 ? "not found, using defaults" : "invalid, using defaults");

	if (server_open(&a->srv, a->epfd, socket_path, on_command, on_connect, a))
		return 1;

	/* 设备打开失败不退出：界面能看到具体哪个设备出错，其余功能照常 */
	a->prox_state = -1;
	a->ap_ok = !ap_open(&a->ap, a->ap_err);
	if (!a->ap_ok)
		dlog("%s", a->ap_err);
	else if (a->ap.event_fd >= 0)
		epoll_add(a, a->ap.event_fd, &a->w_ap_event, W_AP_EVENT);

	a->imu_ok = !imu_open(&a->imu, a->cfg.imu_rate_hz, IMU_DRAINS_PER_SEC, a->imu_err);
	if (!a->imu_ok)
		dlog("%s", a->imu_err);
	else
		dlog("ICM20608 %s at %d Hz, record %d bytes", a->imu.dir, a->imu.rate_hz,
		     a->imu.record_size);

	a->led_fd = led_open(LED_DEVICE);
	if (a->led_fd < 0 || set_led(a, false, err)) {
		dlog("LED %s unavailable", LED_DEVICE);
		if (a->led_fd >= 0)
			close(a->led_fd);
		a->led_fd = -1;
	}

	a->key_fd = input_open_by_name(KEY_INPUT_NAME, key_path, sizeof(key_path));
	a->key_state = a->key_fd >= 0 ? "ready" : "unavailable";
	if (a->key_fd >= 0)
		epoll_add(a, a->key_fd, &a->w_key, W_KEY);
	else
		dlog("key input \"%s\" not found", KEY_INPUT_NAME);

	sysmon_init(&a->sysmon);
	epoll_add(a, a->signal_fd, &a->w_signal, W_SIGNAL);
	epoll_add(a, a->tick_fd, &a->w_tick, W_TICK);
	epoll_add(a, a->drain_fd, &a->w_drain, W_DRAIN);
	timer_set(a->tick_fd, a->cfg.interval_ms);
	timer_set(a->drain_fd, 1000 / IMU_DRAINS_PER_SEC);

	dlog("running, socket %s, key %s", socket_path, a->key_fd >= 0 ? key_path : "-");
	a->running = true;
	while (a->running) {
		struct epoll_event events[16];
		int n = epoll_wait(a->epfd, events, 16, -1), i;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			dlog("epoll_wait: %s", strerror(errno));
			break;
		}
		for (i = 0; i < n; i++) {
			struct watch *w = events[i].data.ptr;

			if (server_dispatch(a->srv, w, events[i].events))
				continue;
			switch (w->kind) {
			case W_SIGNAL: {
				struct signalfd_siginfo si;

				while (read(a->signal_fd, &si, sizeof(si)) == sizeof(si)) {
					if (si.ssi_signo == SIGHUP) {
						ret = config_load(a->config_path, &a->cfg);
						dlog("SIGHUP: reload config (%s)", ret ? "defaults" : "ok");
						timer_set(a->tick_fd, a->cfg.interval_ms);
						send_config(a, NULL);
					} else {
						dlog("signal %u, shutting down", si.ssi_signo);
						a->running = false;
					}
				}
				break;
			}
			case W_TICK:
				on_tick(a);
				break;
			case W_DRAIN:
				timer_ack(a->drain_fd);
				drain_imu(a);
				break;
			case W_KEY:
				on_key(a);
				break;
			case W_AP_EVENT:
				on_ap_event(a);
				break;
			}
		}
	}

	/* 收尾：停录制（刷盘）、关灯、关 IIO 缓冲区和接近中断、删套接字和 pid 文件 */
	stop_recording(a, NULL);
	if (a->led_fd >= 0)
		led_set(a->led_fd, false);
	if (a->imu_ok)
		imu_close(&a->imu);
	if (a->ap_ok)
		ap_close(&a->ap);
	server_close(a->srv);
	unlink(pid_path);
	close(pid_fd);
	dlog("stopped");
	return 0;
}
