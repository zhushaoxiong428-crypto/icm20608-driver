/*
 * 监管进程（master/worker）与硬件看门狗
 *
 *   父进程（监管）：只做三件事，逻辑简单、几乎不会卡死
 *     1. fork 工作进程，工作进程退出或崩溃后按退避时间重启（1 s 起，翻倍，最多 30 s；
 *        稳定运行 60 s 以上再崩溃时从 1 s 重新开始）
 *     2. 读工作进程的心跳管道：超过 HEARTBEAT_TIMEOUT 没有心跳视为卡死，SIGKILL 后重启
 *     3. 每秒喂硬件看门狗（-w）：父进程或整个系统卡死时，看门狗超时复位整板
 *   工作进程：原来的 devmond 主循环，每 2 s 往心跳管道写 1 字节
 *
 * 分层的意义：工作进程的问题（崩溃、卡死）只重启进程，约 1 秒恢复；
 * 只有父进程或系统本身出问题时才复位整块板子。
 * 停止：向父进程发 SIGTERM（kill $(cat /tmp/devmond.pid)），父进程转发给工作进程、等它收尾，
 * 再向看门狗写魔术字符 'V' 正常关闭（未开启 NOWAYOUT 时驱动接手喂狗，不会复位）。
 */
#include "devmond.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HEARTBEAT_TIMEOUT_S	10
#define BACKOFF_MAX_S		30
#define STABLE_RUN_S		60
#define STOP_TIMEOUT_S		5	/* SIGTERM 后工作进程还不退出就 SIGKILL */

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int open_watchdog(int timeout_s)
{
	int fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);

	if (fd < 0) {
		dlog("open /dev/watchdog: %s", strerror(errno));
		return -1;
	}
	if (ioctl(fd, WDIOC_SETTIMEOUT, &timeout_s))
		dlog("watchdog set timeout %d s: %s", timeout_s, strerror(errno));
	ioctl(fd, WDIOC_GETTIMEOUT, &timeout_s);
	dlog("hardware watchdog armed, timeout %d s", timeout_s);
	return fd;
}

/* 写 'V' 后关闭：通知驱动这是有意停止（magic close），不会触发复位 */
static void close_watchdog(int fd)
{
	if (fd < 0)
		return;
	if (write(fd, "V", 1) != 1)
		dlog("watchdog magic close: %s", strerror(errno));
	close(fd);
	dlog("hardware watchdog disarmed");
}

static void describe_exit(int status, char *buf, size_t len)
{
	if (WIFSIGNALED(status))
		snprintf(buf, len, "killed by signal %d (%s)%s", WTERMSIG(status),
			 strsignal(WTERMSIG(status)), WCOREDUMP(status) ? ", core dumped" : "");
	else
		snprintf(buf, len, "exited with status %d", WEXITSTATUS(status));
}

/*
 * 返回值只出现在工作进程里：心跳管道的写端。父进程在这里循环直到收到 SIGTERM，然后 exit。
 * 调用前 SIGINT/SIGTERM/SIGHUP/SIGCHLD 已被阻塞。
 */
int supervisor_run(int watchdog_timeout_s, int pid_fd, const char *pid_path)
{
	struct epoll_event ev = { .events = EPOLLIN };
	int epfd, sig_fd, tick_fd, restart_fd, wd_fd = -1, hb[2] = { -1, -1 };
	struct itimerspec one_second = { { 1, 0 }, { 1, 0 } };
	double started = 0, last_beat = 0, stop_time = 0;
	int backoff = 1, restarts = 0;
	bool stopping = false;
	pid_t worker = -1;
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGCHLD);

	epfd = epoll_create1(EPOLL_CLOEXEC);
	sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	restart_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (epfd < 0 || sig_fd < 0 || tick_fd < 0 || restart_fd < 0) {
		dlog("supervisor setup: %s", strerror(errno));
		exit(1);
	}
	ev.data.fd = sig_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, sig_fd, &ev);
	ev.data.fd = tick_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, tick_fd, &ev);
	ev.data.fd = restart_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, restart_fd, &ev);
	timerfd_settime(tick_fd, 0, &one_second, NULL);

	if (watchdog_timeout_s > 0)
		wd_fd = open_watchdog(watchdog_timeout_s);

	for (;;) {
		struct epoll_event events[4];
		int n, i;

		/* 需要时启动工作进程 */
		if (worker < 0 && !stopping) {
			struct itimerspec armed;

			timerfd_gettime(restart_fd, &armed);
			if (armed.it_value.tv_sec == 0 && armed.it_value.tv_nsec == 0) {
				if (hb[0] >= 0) {
					epoll_ctl(epfd, EPOLL_CTL_DEL, hb[0], NULL);
					close(hb[0]);
				}
				if (pipe2(hb, O_CLOEXEC | O_NONBLOCK)) {
					dlog("pipe: %s", strerror(errno));
					exit(1);
				}
				worker = fork();
				if (worker == 0) {
					/* 工作进程：父进程退出时一起退出，不留孤儿；关掉只属于父进程的 fd */
					prctl(PR_SET_PDEATHSIG, SIGTERM);
					close(hb[0]);
					close(epfd);
					close(sig_fd);
					close(tick_fd);
					close(restart_fd);
					if (wd_fd >= 0)
						close(wd_fd);	/* 不是 magic close，但父进程仍持有并喂狗 */
					close(pid_fd);
					return hb[1];
				}
				close(hb[1]);
				if (worker < 0) {
					dlog("fork: %s", strerror(errno));
					exit(1);
				}
				ev.data.fd = hb[0];
				epoll_ctl(epfd, EPOLL_CTL_ADD, hb[0], &ev);
				started = last_beat = now_s();
				dlog("worker started (pid %d)", worker);
			}
		}

		n = epoll_wait(epfd, events, 4, -1);
		if (n < 0 && errno != EINTR) {
			dlog("supervisor epoll_wait: %s", strerror(errno));
			exit(1);
		}
		for (i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == hb[0]) {
				char buf[64];

				while (read(hb[0], buf, sizeof(buf)) > 0)
					;
				last_beat = now_s();
			} else if (fd == tick_fd) {
				uint64_t exp;

				if (read(tick_fd, &exp, sizeof(exp)) < 0 && errno != EAGAIN)
					dlog("read timerfd: %s", strerror(errno));
				if (wd_fd >= 0)
					ioctl(wd_fd, WDIOC_KEEPALIVE, 0);
				if (stopping && worker > 0 && now_s() - stop_time > STOP_TIMEOUT_S) {
					dlog("worker %d did not stop in %d s, killing", worker, STOP_TIMEOUT_S);
					kill(worker, SIGKILL);
				} else if (worker > 0 && now_s() - last_beat > HEARTBEAT_TIMEOUT_S) {
					dlog("worker %d hung (no heartbeat for %d s), killing", worker,
					     HEARTBEAT_TIMEOUT_S);
					kill(worker, SIGKILL);
					last_beat = now_s();	/* 等 SIGCHLD，不重复杀 */
				}
			} else if (fd == restart_fd) {
				uint64_t exp;

				if (read(restart_fd, &exp, sizeof(exp)) < 0 && errno != EAGAIN)
					dlog("read timerfd: %s", strerror(errno));
				/* 下一轮循环开头启动工作进程 */
			} else if (fd == sig_fd) {
				struct signalfd_siginfo si;

				while (read(sig_fd, &si, sizeof(si)) == sizeof(si)) {
					if (si.ssi_signo == SIGCHLD) {
						int status;
						pid_t pid;

						while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
							char why[96];
							double ran = now_s() - started;
							struct itimerspec delay = { { 0, 0 }, { 0, 0 } };

							if (pid != worker)
								continue;
							worker = -1;
							describe_exit(status, why, sizeof(why));
							if (stopping) {
								dlog("worker %s", why);
								continue;
							}
							/* 退避：频繁崩溃时拉长间隔，稳定运行过一阵再崩溃则从头开始 */
							backoff = ran >= STABLE_RUN_S ? 1 :
								  (restarts == 0 ? 1 : (backoff * 2 > BACKOFF_MAX_S ?
											BACKOFF_MAX_S : backoff * 2));
							restarts++;
							dlog("worker %d %s after %.0f s, restart #%d in %d s",
							     pid, why, ran, restarts, backoff);
							delay.it_value.tv_sec = backoff;
							timerfd_settime(restart_fd, 0, &delay, NULL);
						}
					} else if (si.ssi_signo == SIGHUP) {
						if (worker > 0)
							kill(worker, SIGHUP);	/* 重新读配置交给工作进程 */
					} else if (!stopping) {
						dlog("signal %u, stopping worker", si.ssi_signo);
						stopping = true;
						stop_time = now_s();
						if (worker > 0)
							kill(worker, SIGTERM);
					}
				}
			}
		}

		if (stopping && worker < 0)
			break;
	}

	close_watchdog(wd_fd);
	unlink(pid_path);
	close(pid_fd);
	dlog("supervisor stopped");
	exit(0);
}
