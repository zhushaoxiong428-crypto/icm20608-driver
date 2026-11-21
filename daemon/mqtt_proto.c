/*
 * MQTT 3.1.1 报文编解码与协议行 -> JSON（纯函数，见 mqtt.h）
 * 报文格式：固定报头（类型/标志 1 字节 + 剩余长度 1–4 字节）+ 可变报头 + 载荷；
 * 字符串为 2 字节大端长度 + UTF-8 内容。
 */
#include "mqtt.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

size_t mqtt_encode_len(uint8_t *out, uint32_t len)
{
	size_t n = 0;

	if (len > MQTT_MAX_REMAINING)
		return 0;
	do {
		uint8_t b = len % 128;

		len /= 128;
		out[n++] = len ? b | 0x80 : b;
	} while (len);
	return n;
}

int mqtt_decode_len(const uint8_t *p, size_t avail, uint32_t *len, size_t *used)
{
	uint32_t v = 0, mult = 1;
	size_t i;

	for (i = 0; i < 4; i++) {
		if (i >= avail)
			return 1;
		v += (p[i] & 0x7f) * mult;
		if (!(p[i] & 0x80)) {
			*len = v;
			*used = i + 1;
			return 0;
		}
		mult *= 128;
	}
	return -1;
}

/* 简单的写缓冲：越界后 ok 变 false，最后统一检查 */
struct wbuf {
	uint8_t *p;
	size_t cap, len;
	bool ok;
};

static void put_u8(struct wbuf *w, uint8_t v)
{
	if (w->len + 1 > w->cap) {
		w->ok = false;
		return;
	}
	w->p[w->len++] = v;
}

static void put_u16(struct wbuf *w, uint16_t v)
{
	put_u8(w, v >> 8);
	put_u8(w, v & 0xff);
}

static void put_bytes(struct wbuf *w, const void *src, size_t n)
{
	if (w->len + n > w->cap) {
		w->ok = false;
		return;
	}
	memcpy(w->p + w->len, src, n);
	w->len += n;
}

static void put_str(struct wbuf *w, const char *s, size_t n)
{
	if (n > 0xffff) {
		w->ok = false;
		return;
	}
	put_u16(w, n);
	put_bytes(w, s, n);
}

/*
 * 先在 buf 开头留 5 字节（固定报头最长 5 字节）写可变报头和载荷，
 * 知道剩余长度后再把固定报头写到紧挨着内容的前面，最后整体前移
 */
#define HDR_MAX 5

static size_t finish(struct wbuf *w, uint8_t first_byte)
{
	uint8_t hdr[HDR_MAX];
	size_t body = w->len - HDR_MAX, hn;

	if (!w->ok)
		return 0;
	hdr[0] = first_byte;
	hn = 1 + mqtt_encode_len(hdr + 1, body);
	if (hn == 1)
		return 0;
	memmove(w->p + hn, w->p + HDR_MAX, body);
	memcpy(w->p, hdr, hn);
	return hn + body;
}

static void wbuf_init(struct wbuf *w, uint8_t *buf, size_t cap)
{
	w->p = buf;
	w->cap = cap;
	w->len = HDR_MAX;
	w->ok = cap >= HDR_MAX;
}

size_t mqtt_build_connect(uint8_t *buf, size_t cap, const char *client_id, uint16_t keepalive_s,
			  const char *will_topic, const char *will_msg)
{
	struct wbuf w;
	uint8_t flags = 0x02;	/* clean session */

	if (will_topic)
		flags |= 0x04 | (1 << 3) | 0x20;	/* will flag、will QoS 1、will retain */
	wbuf_init(&w, buf, cap);
	put_str(&w, "MQTT", 4);		/* 协议名 */
	put_u8(&w, 4);			/* 协议级别 4 = 3.1.1 */
	put_u8(&w, flags);
	put_u16(&w, keepalive_s);
	put_str(&w, client_id, strlen(client_id));
	if (will_topic) {
		put_str(&w, will_topic, strlen(will_topic));
		put_str(&w, will_msg, strlen(will_msg));
	}
	return finish(&w, MQTT_CONNECT << 4);
}

size_t mqtt_build_publish(uint8_t *buf, size_t cap, const char *topic,
			  const void *payload, size_t plen, bool retain)
{
	struct wbuf w;

	wbuf_init(&w, buf, cap);
	put_str(&w, topic, strlen(topic));
	put_bytes(&w, payload, plen);	/* QoS 0：没有报文标识符 */
	return finish(&w, MQTT_PUBLISH << 4 | (retain ? 1 : 0));
}

size_t mqtt_build_subscribe(uint8_t *buf, size_t cap, uint16_t packet_id,
			    const char *topic, uint8_t qos)
{
	struct wbuf w;

	wbuf_init(&w, buf, cap);
	put_u16(&w, packet_id);
	put_str(&w, topic, strlen(topic));
	put_u8(&w, qos);
	return finish(&w, MQTT_SUBSCRIBE << 4 | 0x02);	/* SUBSCRIBE 的标志位规定为 0010 */
}

size_t mqtt_build_puback(uint8_t *buf, size_t cap, uint16_t packet_id)
{
	struct wbuf w;

	wbuf_init(&w, buf, cap);
	put_u16(&w, packet_id);
	return finish(&w, MQTT_PUBACK << 4);
}

size_t mqtt_build_pingreq(uint8_t *buf, size_t cap)
{
	struct wbuf w;

	wbuf_init(&w, buf, cap);
	return finish(&w, MQTT_PINGREQ << 4);
}

int mqtt_parse_publish(uint8_t flags, const uint8_t *body, size_t len, struct mqtt_publish *out)
{
	size_t pos;

	memset(out, 0, sizeof(*out));
	out->qos = (flags >> 1) & 3;
	out->retain = flags & 1;
	out->dup = flags & 8;
	if (out->qos == 3 || len < 2)
		return -1;
	out->topic_len = (size_t)body[0] << 8 | body[1];
	pos = 2 + out->topic_len;
	if (pos > len || out->topic_len == 0)
		return -1;
	out->topic = (const char *)body + 2;
	if (out->qos) {
		if (pos + 2 > len)
			return -1;
		out->packet_id = (uint16_t)(body[pos] << 8 | body[pos + 1]);
		if (out->packet_id == 0)
			return -1;
		pos += 2;
	}
	out->payload = body + pos;
	out->payload_len = len - pos;
	return 0;
}

/* JSON 数字的语法：-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
static bool is_json_number(const char *s, size_t n)
{
	size_t i = 0;

	if (i < n && s[i] == '-')
		i++;
	if (i >= n || !isdigit((unsigned char)s[i]))
		return false;
	if (s[i] == '0')
		i++;
	else
		while (i < n && isdigit((unsigned char)s[i]))
			i++;
	if (i < n && s[i] == '.') {
		i++;
		if (i >= n || !isdigit((unsigned char)s[i]))
			return false;
		while (i < n && isdigit((unsigned char)s[i]))
			i++;
	}
	if (i < n && (s[i] == 'e' || s[i] == 'E')) {
		i++;
		if (i < n && (s[i] == '+' || s[i] == '-'))
			i++;
		if (i >= n || !isdigit((unsigned char)s[i]))
			return false;
		while (i < n && isdigit((unsigned char)s[i]))
			i++;
	}
	return i == n;
}

struct jbuf {
	char *p;
	size_t cap, len;
	bool ok;
};

static void jput(struct jbuf *j, const char *s, size_t n)
{
	if (j->len + n + 1 > j->cap) {
		j->ok = false;
		return;
	}
	memcpy(j->p + j->len, s, n);
	j->len += n;
	j->p[j->len] = '\0';
}

/* 字符串值：转义引号、反斜杠和控制字符 */
static void jput_string(struct jbuf *j, const char *s, size_t n)
{
	size_t i;

	jput(j, "\"", 1);
	for (i = 0; i < n; i++) {
		unsigned char c = s[i];
		char esc[8];

		if (c == '"' || c == '\\') {
			esc[0] = '\\';
			esc[1] = c;
			jput(j, esc, 2);
		} else if (c < 0x20) {
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			jput(j, esc, 6);
		} else {
			jput(j, (const char *)&s[i], 1);
		}
	}
	jput(j, "\"", 1);
}

int mqtt_line_to_json(const char *line, int64_t ts_ms, char *type, size_t type_cap,
		      char *json, size_t json_cap)
{
	struct jbuf j = { json, json_cap, 0, json_cap > 0 };
	const char *p = line, *sp;
	size_t tn, i;
	bool first = true;
	char ts[32];

	sp = strchr(p, ' ');
	tn = sp ? (size_t)(sp - p) : strlen(p);
	if (tn == 0 || tn >= type_cap)
		return -1;
	for (i = 0; i < tn; i++) {
		if (!isalnum((unsigned char)p[i]) && p[i] != '_')
			return -1;	/* 类型要做主题名，只允许字母数字下划线 */
		type[i] = tolower((unsigned char)p[i]);
	}
	type[tn] = '\0';

	jput(&j, "{", 1);
	p = sp;
	while (p && *p) {
		const char *key, *eq, *val, *end;

		while (*p == ' ')
			p++;
		if (!*p)
			break;
		key = p;
		eq = strchr(p, '=');
		end = strchr(p, ' ');
		if (!eq || (end && end < eq))
			return -1;	/* 每个字段都必须是 key=value */
		val = eq + 1;
		/* 协议规定 msg= 总在行尾，值可以带空格 */
		if (eq - key == 3 && !strncmp(key, "msg", 3))
			end = val + strlen(val);
		else if (!end)
			end = val + strlen(val);

		if (!first)
			jput(&j, ",", 1);
		first = false;
		jput_string(&j, key, eq - key);
		jput(&j, ":", 1);
		if (is_json_number(val, end - val))
			jput(&j, val, end - val);
		else
			jput_string(&j, val, end - val);
		p = end;
	}
	snprintf(ts, sizeof(ts), "%s\"ts\":%" PRId64 "}", first ? "" : ",", ts_ms);
	jput(&j, ts, strlen(ts));
	return j.ok ? 0 : -1;
}
