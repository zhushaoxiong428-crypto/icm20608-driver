/*
 * mqtt_proto.c 的单元测试，在开发主机上编译运行：make daemon-test
 * 报文的期望字节按 MQTT 3.1.1 规范手算。
 */
#include "mqtt.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void check_bytes(const char *what, const uint8_t *got, size_t n,
			const uint8_t *want, size_t wn)
{
	size_t i;

	if (n == wn && !memcmp(got, want, n))
		return;
	printf("FAIL %s: got %zu bytes:", what, n);
	for (i = 0; i < n; i++)
		printf(" %02x", got[i]);
	printf("\n     want %zu bytes:", wn);
	for (i = 0; i < wn; i++)
		printf(" %02x", want[i]);
	printf("\n");
	failures++;
}

static void test_remaining_length(void)
{
	static const struct { uint32_t v; uint8_t b[4]; size_t n; } cases[] = {
		{ 0, { 0x00 }, 1 },
		{ 127, { 0x7f }, 1 },
		{ 128, { 0x80, 0x01 }, 2 },
		{ 16383, { 0xff, 0x7f }, 2 },
		{ 16384, { 0x80, 0x80, 0x01 }, 3 },
		{ 2097151, { 0xff, 0xff, 0x7f }, 3 },
		{ 2097152, { 0x80, 0x80, 0x80, 0x01 }, 4 },
		{ 268435455, { 0xff, 0xff, 0xff, 0x7f }, 4 },
	};
	static const uint8_t five[] = { 0x80, 0x80, 0x80, 0x80, 0x01 };
	uint8_t out[4];
	uint32_t len;
	size_t i, used;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t n = mqtt_encode_len(out, cases[i].v);

		check_bytes("encode_len", out, n, cases[i].b, cases[i].n);
		CHECK(mqtt_decode_len(cases[i].b, cases[i].n, &len, &used) == 0);
		CHECK(len == cases[i].v && used == cases[i].n);
		/* 少给一个字节：要求继续等数据 */
		CHECK(mqtt_decode_len(cases[i].b, cases[i].n - 1, &len, &used) == 1);
	}
	CHECK(mqtt_encode_len(out, 268435456) == 0);
	CHECK(mqtt_decode_len(five, sizeof(five), &len, &used) == -1);
}

static void test_build(void)
{
	static const uint8_t connect[] = {
		0x10, 28,
		0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04,
		0x2e,			/* will retain | will QoS 1 | will flag | clean session */
		0x00, 30,		/* keepalive 30 s */
		0x00, 0x02, 'c', '1',
		0x00, 0x03, 't', '/', 's',
		0x00, 0x07, 'o', 'f', 'f', 'l', 'i', 'n', 'e',
	};
	static const uint8_t connect_nowill[] = {
		0x10, 14, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 60,
		0x00, 0x02, 'c', '1',
	};
	static const uint8_t publish[] = { 0x31, 7, 0x00, 0x03, 'a', '/', 'b', 'h', 'i' };
	static const uint8_t subscribe[] = { 0x82, 6, 0x00, 0x01, 0x00, 0x01, 'x', 0x01 };
	static const uint8_t puback[] = { 0x40, 2, 0x12, 0x34 };
	static const uint8_t pingreq[] = { 0xc0, 0 };
	uint8_t buf[512], big[300];
	uint32_t len;
	size_t n, used;

	n = mqtt_build_connect(buf, sizeof(buf), "c1", 30, "t/s", "offline");
	check_bytes("connect", buf, n, connect, sizeof(connect));
	n = mqtt_build_connect(buf, sizeof(buf), "c1", 60, NULL, NULL);
	check_bytes("connect without will", buf, n, connect_nowill, sizeof(connect_nowill));
	n = mqtt_build_publish(buf, sizeof(buf), "a/b", "hi", 2, true);
	check_bytes("publish", buf, n, publish, sizeof(publish));
	n = mqtt_build_subscribe(buf, sizeof(buf), 1, "x", 1);
	check_bytes("subscribe", buf, n, subscribe, sizeof(subscribe));
	n = mqtt_build_puback(buf, sizeof(buf), 0x1234);
	check_bytes("puback", buf, n, puback, sizeof(puback));
	n = mqtt_build_pingreq(buf, sizeof(buf));
	check_bytes("pingreq", buf, n, pingreq, sizeof(pingreq));
	/* 与报文一样大的 buf 不够：要多留 5 字节报头空间（曾经因此发不出 PINGREQ） */
	CHECK(mqtt_build_pingreq(buf, 2) == 0);
	CHECK(mqtt_build_pingreq(buf, 7) == 2);

	/* 剩余长度超过 127：固定报头变成 3 字节，内容紧跟其后 */
	memset(big, 'z', sizeof(big));
	n = mqtt_build_publish(buf, sizeof(buf), "a/b", big, sizeof(big), false);
	CHECK(n == 3 + 2 + 3 + sizeof(big));
	CHECK(buf[0] == 0x30);
	CHECK(mqtt_decode_len(buf + 1, n - 1, &len, &used) == 0 && used == 2);
	CHECK(len == 2 + 3 + sizeof(big));
	CHECK(buf[3] == 0 && buf[4] == 3 && !memcmp(buf + 5, "a/b", 3) && buf[8] == 'z');

	/* 空间不够返回 0，而不是写出半个报文 */
	CHECK(mqtt_build_publish(buf, 8, "a/b", "hi", 2, true) == 0);
	CHECK(mqtt_build_publish(buf, sizeof(publish), "a/b", "hi", 2, true) == 0);	/* 需要预留 5 字节报头 */
	CHECK(mqtt_build_publish(buf, sizeof(publish) + 3, "a/b", "hi", 2, true) == sizeof(publish));
}

static void test_parse_publish(void)
{
	static const uint8_t qos1[] = { 0x00, 0x03, 'a', '/', 'c', 0x00, 0x05, 'L', 'E', 'D', ' ', 'o', 'n' };
	static const uint8_t qos0[] = { 0x00, 0x01, 'x', 'p' };
	static const uint8_t bad_topic[] = { 0x00, 0x09, 'a' };
	static const uint8_t zero_id[] = { 0x00, 0x01, 'x', 0x00, 0x00 };
	struct mqtt_publish p;

	CHECK(mqtt_parse_publish(0x02, qos1, sizeof(qos1), &p) == 0);
	CHECK(p.qos == 1 && !p.retain && p.packet_id == 5);
	CHECK(p.topic_len == 3 && !memcmp(p.topic, "a/c", 3));
	CHECK(p.payload_len == 6 && !memcmp(p.payload, "LED on", 6));

	CHECK(mqtt_parse_publish(0x01, qos0, sizeof(qos0), &p) == 0);
	CHECK(p.qos == 0 && p.retain && p.packet_id == 0 && p.payload_len == 1);

	CHECK(mqtt_parse_publish(0x00, bad_topic, sizeof(bad_topic), &p) == -1);
	CHECK(mqtt_parse_publish(0x06, qos0, sizeof(qos0), &p) == -1);	/* QoS 3 */
	CHECK(mqtt_parse_publish(0x02, zero_id, sizeof(zero_id), &p) == -1);
	CHECK(mqtt_parse_publish(0x00, qos0, 1, &p) == -1);
}

static void check_json(const char *line, const char *want_type, const char *want_json)
{
	char type[32], json[512];

	if (mqtt_line_to_json(line, 5, type, sizeof(type), json, sizeof(json))) {
		printf("FAIL line_to_json(\"%s\") returned error\n", line);
		failures++;
		return;
	}
	if (strcmp(type, want_type) || strcmp(json, want_json)) {
		printf("FAIL line_to_json(\"%s\"):\n     got  %s %s\n     want %s %s\n",
		       line, type, json, want_type, want_json);
		failures++;
	}
}

static void test_line_to_json(void)
{
	char type[32], json[16];

	check_json("IMU n=100 rate=100.1 ax=-0.029 az=1.007", "imu",
		   "{\"n\":100,\"rate\":100.1,\"ax\":-0.029,\"az\":1.007,\"ts\":5}");
	check_json("LED state=on auto=0", "led", "{\"state\":\"on\",\"auto\":0,\"ts\":5}");
	check_json("CONFIG on=20 off=50 path=/opt/device-monitor/device_monitor.ini", "config",
		   "{\"on\":20,\"off\":50,\"path\":\"/opt/device-monitor/device_monitor.ini\",\"ts\":5}");
	/* msg= 取到行尾，含空格；引号和反斜杠要转义 */
	check_json("AP_ERR msg=open \"x\\y\" failed: No such file", "ap_err",
		   "{\"msg\":\"open \\\"x\\\\y\\\" failed: No such file\",\"ts\":5}");
	check_json("REC state=error msg=disk full", "rec",
		   "{\"state\":\"error\",\"msg\":\"disk full\",\"ts\":5}");
	check_json("PONG", "pong", "{\"ts\":5}");
	/* 只有符合 JSON 数字语法的才输出为数字 */
	check_json("X a=007 b=1e5 c=-0 d=1. e=.5 f=0x10 g=nan h=1.5E-3 i=", "x",
		   "{\"a\":\"007\",\"b\":1e5,\"c\":-0,\"d\":\"1.\",\"e\":\".5\",\"f\":\"0x10\","
		   "\"g\":\"nan\",\"h\":1.5E-3,\"i\":\"\",\"ts\":5}");

	CHECK(mqtt_line_to_json("IMU x", 5, type, sizeof(type), json, sizeof(json)) == -1);
	CHECK(mqtt_line_to_json("", 5, type, sizeof(type), json, sizeof(json)) == -1);
	CHECK(mqtt_line_to_json("A-B x=1", 5, type, sizeof(type), json, sizeof(json)) == -1);
	CHECK(mqtt_line_to_json("IMU n=100 rate=100.1", 5, type, sizeof(type), json, sizeof(json)) == -1);
	CHECK(mqtt_line_to_json("IMU n=1", 5, type, 3, json, sizeof(json)) == -1);	/* 类型放不下 */
}

int main(void)
{
	test_remaining_length();
	test_build();
	test_parse_publish();
	test_line_to_json();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("mqtt_proto: all tests passed\n");
	return 0;
}
