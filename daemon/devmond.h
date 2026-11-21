/*
 * devmond：设备监测采集守护进程（不依赖 Qt）
 *
 * 负责传感器采集（ICM20608、AP3216C）、LED 与按键、自动亮灯、参数保存和 CSV 录制，
 * 通过 Unix 域套接字向任意多个客户端（Qt 界面、devmonctl）推送数据、接收命令。
 * 协议见 protocol.md。
 */
#ifndef DEVMOND_H
#define DEVMOND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DEVMOND_PROTO_VERSION	1
#define DEVMOND_DEFAULT_SOCKET	"/tmp/devmond.sock"
#define DEVMOND_DEFAULT_PIDFILE	"/tmp/devmond.pid"

#define ERRLEN			160

/* ---------- log.c ---------- */
void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int64_t realtime_ns(void);
/* 纳秒时间戳 -> 本地时间 "2024-12-04T06:46:25.112"（与 Qt::ISODateWithMs 一致） */
void format_iso_ms(int64_t ns, char *buf, size_t len);

/* ---------- iio.c：IIO sysfs 辅助 ---------- */
int iio_find_device(const char *const *names, char *dir, size_t dirlen,
		    char *node, size_t nodelen);
int iio_write_attr(const char *dir, const char *attr, const char *value);
int iio_read_attr(const char *dir, const char *attr, char *buf, size_t len);
int iio_read_double(const char *dir, const char *attr, double *out);

/* ---------- imu.c：ICM20608 IIO 缓冲区 ---------- */
struct imu_sample {
	double accel[3];	/* m/s^2 */
	double gyro[3];		/* rad/s */
	double temp_c;
	int64_t ts_ns;		/* 驱动在数据就绪中断上半部记录，CLOCK_REALTIME */
};

struct scan_elem {
	char name[40];
	int index;
	bool big_endian;
	bool is_signed;
	int realbits;
	int bytes;
	int shift;
	int offset;
};

enum imu_field {
	IMU_AX, IMU_AY, IMU_AZ, IMU_TEMP, IMU_GX, IMU_GY, IMU_GZ, IMU_TS, IMU_FIELDS
};

struct imu {
	char dir[128];
	char node[64];
	int fd;
	int rate_hz;
	int record_size;
	struct scan_elem elems[16];
	int nelems;
	int field[IMU_FIELDS];	/* 字段 -> elems 下标，-1 表示没有 */
	double accel_scale, gyro_scale, temp_scale, temp_offset;
	uint8_t *buf;
	size_t buflen;
};

typedef void (*imu_sample_cb)(const struct imu_sample *s, void *ctx);

int imu_open(struct imu *imu, int rate_hz, int drains_per_sec, char *err);
void imu_close(struct imu *imu);
/* 非阻塞读走缓冲区里全部记录，逐个回调；返回样本数，出错返回 -1 */
int imu_drain(struct imu *imu, imu_sample_cb cb, void *ctx, char *err);

/* ---------- ap.c：AP3216C ---------- */
struct ap {
	char dir[128];
	int ir_fd, als_fd, ps_fd, event_fd;
	double lux_per_count;
};

typedef void (*ap_event_cb)(bool near, int64_t ts_ns, void *ctx);

int ap_open(struct ap *ap, char *err);
void ap_close(struct ap *ap);
/* flags 为 ap3216c_data.h 的 AP3216C_*_VALID；单通道无效（IR 溢出）不算错误 */
int ap_read(struct ap *ap, int *ir, int *als, int *ps, int *flags, char *err);
int ap_read_events(struct ap *ap, ap_event_cb cb, void *ctx, char *err);

/* ---------- io.c：LED、按键 ---------- */
int led_open(const char *path);
int led_set(int fd, bool on);
/* 按 input 设备名查找并打开（eventX 编号随驱动加载顺序变化） */
int input_open_by_name(const char *name, char *path, size_t len);

/* ---------- config.c：device_monitor.ini ---------- */
struct config {
	int on_threshold;	/* ALS 原始值 <= 该值开灯 */
	int off_threshold;	/* ALS 原始值 >= 该值关灯 */
	int interval_ms;	/* AP3216C 采集与发布周期 */
	int imu_rate_hz;	/* ICM20608 采样率，修改后重启生效 */
};

void config_defaults(struct config *c);
bool config_valid(const struct config *c, char *err);
/* 0：读到合法配置；1：文件不存在（用默认值）；-1：内容非法（用默认值） */
int config_load(const char *path, struct config *c);
int config_save(const char *path, const struct config *c, char *err);

/* ---------- recorder.c：CSV 录制 ---------- */
struct recorder {
	FILE *ap;
	FILE *imu;
	bool on;
	int64_t start_ns;
	char dir[200];
};

int recorder_start(struct recorder *r, const char *dir, char *err);
void recorder_stop(struct recorder *r);
int recorder_ap(struct recorder *r, int64_t ts_ns, int ir, int als, double lux,
		int ps, int flags);
int recorder_imu(struct recorder *r, const struct imu_sample *s);
int recorder_flush(struct recorder *r, char *err);

/* ---------- sysmon.c：系统状态（/proc、/sys） ---------- */
#define SYSMON_SELF_WINDOW	11	/* devmond 自身 CPU 按最近 10 个周期计算 */

struct sysmon {
	uint64_t cpu_total, cpu_idle;	/* 上次 /proc/stat */
	/*
	 * devmond utime + stime 的历史：CPU 时间按时钟节拍（CLK_TCK=100，10 ms）计，
	 * 每秒采一次分辨率只有 1 %，占用 0.5 % 时读数在 0 和 1 之间跳；用 10 秒窗口提高到 0.1 %
	 */
	uint64_t self_ticks[SYSMON_SELF_WINDOW];
	int64_t self_ns[SYSMON_SELF_WINDOW];
	int self_count;
	uint64_t rx_bytes, tx_bytes;	/* 上次除 lo 外网卡收发字节 */
	int64_t last_ns;
	long clk_tck;
};

struct sysmon_sample {
	double cpu_pct;			/* 整机 CPU 占用 */
	double self_cpu_pct;		/* devmond 自身 */
	double mem_used_mb, mem_total_mb;
	double load1;
	long uptime_s;
	double temp_c;			/* SoC 温度 */
	bool temp_ok;
	double rx_kbps, tx_kbps;	/* KB/s */
	double disk_free_mb, disk_total_mb;
	bool disk_ok;
};

void sysmon_init(struct sysmon *m);
void sysmon_sample(struct sysmon *m, const char *disk_path, struct sysmon_sample *s);

/* ---------- server.c：Unix 域套接字，文本行协议 ---------- */
struct client;
struct server;

typedef void (*command_cb)(struct server *srv, struct client *c, char *line, void *ctx);
typedef void (*connect_cb)(struct server *srv, struct client *c, void *ctx);

int server_open(struct server **out, int epfd, const char *path,
		command_cb on_command, connect_cb on_connect, void *ctx);
void server_close(struct server *srv);
/* epoll 返回的 data.ptr 属于 server（监听或客户端）时处理并返回 true */
bool server_dispatch(struct server *srv, void *ptr, uint32_t events);
void client_send(struct server *srv, struct client *c, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void server_broadcast(struct server *srv, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int server_client_count(const struct server *srv);

#endif /* DEVMOND_H */
