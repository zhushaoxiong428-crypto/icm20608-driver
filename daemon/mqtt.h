/*
 * MQTT 3.1.1 客户端（只实现本项目用到的部分）
 *
 * mqtt_proto.c：报文编解码和协议行 -> JSON，纯函数，不碰 fd，可在主机上单元测试
 * mqtt.c：     与 broker 的连接（非阻塞，挂在 devmond 的 epoll 上）和协议桥接
 *
 * 用到的报文：CONNECT（带遗嘱）、CONNACK、PUBLISH（发 QoS 0；收 QoS 0/1）、PUBACK、
 * SUBSCRIBE、SUBACK、PINGREQ、PINGRESP。不实现：发 QoS 1/2、会话保持、TLS、认证。
 */
#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 报文类型（固定报头第一字节的高 4 位） */
enum {
	MQTT_CONNECT = 1, MQTT_CONNACK = 2, MQTT_PUBLISH = 3, MQTT_PUBACK = 4,
	MQTT_SUBSCRIBE = 8, MQTT_SUBACK = 9, MQTT_PINGREQ = 12, MQTT_PINGRESP = 13,
	MQTT_DISCONNECT = 14,
};

#define MQTT_MAX_REMAINING	268435455u	/* 剩余长度 4 字节变长编码的上限 */

/* ---------- mqtt_proto.c ---------- */

/* 剩余长度编码：每字节 7 位，最高位表示后面还有；返回字节数（1–4），len 超限返回 0 */
size_t mqtt_encode_len(uint8_t *out, uint32_t len);
/* 解码：返回 1 = 数据还不够，0 = 成功（*len、*used 有效），-1 = 格式错误（超过 4 字节） */
int mqtt_decode_len(const uint8_t *p, size_t avail, uint32_t *len, size_t *used);

/*
 * 以下 build 函数把整个报文写进 buf，返回报文长度；cap 不够或参数超限返回 0。
 * 注意 cap 要比报文多留 5 字节：先在开头预留固定报头的最大长度，写完内容再前移
 * （所以 2 字节的 PINGREQ 也需要 7 字节的 buf）。
 * clean session 固定为 1；遗嘱（will_topic 非 NULL 时）QoS 1、retain。
 */
size_t mqtt_build_connect(uint8_t *buf, size_t cap, const char *client_id, uint16_t keepalive_s,
			  const char *will_topic, const char *will_msg);
size_t mqtt_build_publish(uint8_t *buf, size_t cap, const char *topic,
			  const void *payload, size_t plen, bool retain);	/* QoS 0 */
size_t mqtt_build_subscribe(uint8_t *buf, size_t cap, uint16_t packet_id,
			    const char *topic, uint8_t qos);
size_t mqtt_build_puback(uint8_t *buf, size_t cap, uint16_t packet_id);
size_t mqtt_build_pingreq(uint8_t *buf, size_t cap);

/* 收到的 PUBLISH（固定报头之后的部分）；指针指向 body 内部，topic 不以 0 结尾 */
struct mqtt_publish {
	const char *topic;
	size_t topic_len;
	uint8_t qos;
	bool retain, dup;
	uint16_t packet_id;	/* QoS 0 时为 0 */
	const uint8_t *payload;
	size_t payload_len;
};

/* flags 为固定报头第一字节的低 4 位；格式错误返回 -1 */
int mqtt_parse_publish(uint8_t flags, const uint8_t *body, size_t len, struct mqtt_publish *out);

/*
 * devmond 协议行 -> 主题名和 JSON：
 *   "IMU n=100 rate=100.1 ax=-0.029"  ->  type "imu"，{"n":100,"rate":100.1,"ax":-0.029,"ts":...}
 * 类型转小写作为主题最后一级；看起来像 JSON 数字的值输出为数字，其余为字符串；
 * msg= 按协议取到行尾（可含空格）；末尾追加 "ts"（毫秒，调用者给出）。
 * 返回 0；行格式不对或缓冲区不够返回 -1。
 */
int mqtt_line_to_json(const char *line, int64_t ts_ms, char *type, size_t type_cap,
		      char *json, size_t json_cap);

/* ---------- mqtt.c：连接与桥接 ---------- */
struct server;
struct mqtt;

/*
 * broker 为 "IP" 或 "IP:端口"（默认 1883；只接受数字地址：解析主机名可能阻塞主循环）；
 * prefix 为主题前缀，如 "devmon/alpha"。立即开始连接，失败按退避重试，不影响其他功能。
 */
int mqtt_open(struct mqtt **out, int epfd, struct server *srv, const char *broker,
	      const char *prefix, const char *client_id);
/* epoll 返回的 data.ptr 属于 mqtt（套接字或定时器）时处理并返回 true */
bool mqtt_dispatch(struct mqtt *m, void *ptr, uint32_t events);
/* 不发 DISCONNECT 直接断开：broker 随即发布遗嘱 "offline" */
void mqtt_close(struct mqtt *m);
/* 当前状态（"connecting"、"connected"、"retry in 4 s" 等），广播给界面用 */
void mqtt_status_line(const struct mqtt *m, char *buf, size_t len);

#endif /* MQTT_H */
