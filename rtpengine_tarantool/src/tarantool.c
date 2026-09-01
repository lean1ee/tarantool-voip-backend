/*
 * Copyright (C) 2026 Sipwise GmbH / RTPEngine Project
 *
 * Driver: rtpengine_tarantool - High performance non-blocking Tarantool 3.x IProto driver
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "tarantool.h"
#include "msgpuck.h"

/* --- CERT C MSC06-C Secure Memory Zeroing --- */

static void tnt_memzero_explicit(void *ptr, size_t len)
{
	if (!ptr || len == 0)
		return;
	memset(ptr, 0, len);
#if defined(__GNUC__) || defined(__clang__)
	__asm__ __volatile__("" : : "r"(ptr) : "memory");
#endif
}

/* --- Embedded RFC 3174 SHA-1 Implementation --- */

typedef struct {
	uint32_t state[5];
	uint32_t count[2];
	unsigned char buffer[64];
} tnt_sha1_ctx_t;

#define TNT_SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void tnt_sha1_transform(uint32_t state[5], const unsigned char buffer[64])
{
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
	uint32_t block[80];
	int i;

	for (i = 0; i < 16; i++) {
		block[i] = ((uint32_t)buffer[i * 4] << 24) |
			   ((uint32_t)buffer[i * 4 + 1] << 16) |
			   ((uint32_t)buffer[i * 4 + 2] << 8) |
			   ((uint32_t)buffer[i * 4 + 3]);
	}
	for (i = 16; i < 80; i++) {
		block[i] = TNT_SHA1_ROL(block[i - 3] ^ block[i - 8] ^ block[i - 14] ^ block[i - 16], 1);
	}

	for (i = 0; i < 20; i++) {
		uint32_t temp = TNT_SHA1_ROL(a, 5) + ((b & c) | ((~b) & d)) + e + block[i] + 0x5a827999;
		e = d; d = c; c = TNT_SHA1_ROL(b, 30); b = a; a = temp;
	}
	for (i = 20; i < 40; i++) {
		uint32_t temp = TNT_SHA1_ROL(a, 5) + (b ^ c ^ d) + e + block[i] + 0x6ed9eba1;
		e = d; d = c; c = TNT_SHA1_ROL(b, 30); b = a; a = temp;
	}
	for (i = 40; i < 60; i++) {
		uint32_t temp = TNT_SHA1_ROL(a, 5) + ((b & c) | (b & d) | (c & d)) + e + block[i] + 0x8f1bbcdc;
		e = d; d = c; c = TNT_SHA1_ROL(b, 30); b = a; a = temp;
	}
	for (i = 60; i < 80; i++) {
		uint32_t temp = TNT_SHA1_ROL(a, 5) + (b ^ c ^ d) + e + block[i] + 0xca62c1d6;
		e = d; d = c; c = TNT_SHA1_ROL(b, 30); b = a; a = temp;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

static void tnt_sha1_init(tnt_sha1_ctx_t *context)
{
	context->state[0] = 0x67452301;
	context->state[1] = 0xefcdab89;
	context->state[2] = 0x98badcfe;
	context->state[3] = 0x10325476;
	context->state[4] = 0xc3d2e1f0;
	context->count[0] = 0;
	context->count[1] = 0;
}

static void tnt_sha1_update(tnt_sha1_ctx_t *context, const unsigned char *data, uint32_t len)
{
	uint32_t i, j;

	j = (context->count[0] >> 3) & 63;
	if ((context->count[0] += len << 3) < (len << 3))
		context->count[1]++;
	context->count[1] += (len >> 29);

	if ((j + len) > 63) {
		memcpy(&context->buffer[j], data, (size_t)(i = 64 - j));
		tnt_sha1_transform(context->state, context->buffer);
		for (; i + 63 < len; i += 64) {
			tnt_sha1_transform(context->state, &data[i]);
		}
		j = 0;
	} else {
		i = 0;
	}
	memcpy(&context->buffer[j], &data[i], (size_t)(len - i));
}

static void tnt_sha1_final(unsigned char digest[20], tnt_sha1_ctx_t *context)
{
	uint32_t i;
	unsigned char finalcount[8];

	for (i = 0; i < 8; i++) {
		finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
	}
	tnt_sha1_update(context, (const unsigned char *)"\200", 1);
	while ((context->count[0] & 504) != 448) {
		tnt_sha1_update(context, (const unsigned char *)"\0", 1);
	}
	tnt_sha1_update(context, finalcount, 8);
	for (i = 0; i < 20; i++) {
		digest[i] = (unsigned char)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
	}
	tnt_memzero_explicit(context, sizeof(*context));
}

static void tnt_sha1(const unsigned char *data, uint32_t len, unsigned char digest[20])
{
	tnt_sha1_ctx_t ctx;
	tnt_sha1_init(&ctx);
	tnt_sha1_update(&ctx, data, len);
	tnt_sha1_final(digest, &ctx);
}

/* --- Embedded RFC 4648 Base64 Decoder --- */

static const int8_t tnt_b64_table[256] = {
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
	52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
	-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
	15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
	-1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
	41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
	-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static int tnt_base64_decode(const char *src, size_t slen, unsigned char *dst, size_t dlen)
{
	size_t i = 0, j = 0;
	uint32_t buf = 0;
	int bits = 0;

	for (i = 0; i < slen && src[i] && src[i] != '='; i++) {
		int val = tnt_b64_table[(unsigned char)src[i]];
		if (val < 0)
			continue;
		buf = (buf << 6) | (uint32_t)val;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (j < dlen)
				dst[j++] = (unsigned char)((buf >> bits) & 0xff);
		}
	}
	return (int)j;
}

/* --- Socket Helpers --- */

static int tnt_socket_send_all(int fd, const char *buf, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, buf + sent, len - sent, 0);
		if (n <= 0) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
			return -1;
		}
		sent += (size_t)n;
	}
	return 0;
}

static int tnt_socket_recv_all(int fd, char *buf, size_t len)
{
	size_t recvd = 0;
	while (recvd < len) {
		ssize_t n = recv(fd, buf + recvd, len - recvd, 0);
		if (n <= 0) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
			return -1;
		}
		recvd += (size_t)n;
	}
	return 0;
}

static void tnt_socket_drain(int fd)
{
	char drain_buf[1024];
	ssize_t n;

	if (fd < 0)
		return;

	while ((n = recv(fd, drain_buf, sizeof(drain_buf), MSG_DONTWAIT)) > 0) {
		/* Discard incoming IProto ACKs to prevent socket buffer congestion */
	}
}

/**
 * tnt_auth_scramble - Execute IPROTO_AUTH CHAP-SHA1 handshake against greeting salt
 * @client: Tarantool client instance
 * @salt_b64: Base64-encoded salt string (44 bytes from greeting[64..107])
 * @salt_b64_len: length of salt string
 *
 * Returns 0 on successful authentication, or -1 on error.
 */
static int tnt_auth_scramble(rtpe_tarantool_client_t *client, const char *salt_b64, size_t salt_b64_len)
{
	unsigned char raw_salt[64];
	int raw_salt_len;
	unsigned char h1[TNT_SHA1_DIGEST_SIZE];
	unsigned char h2[TNT_SHA1_DIGEST_SIZE];
	unsigned char step3_in[TNT_SHA1_DIGEST_SIZE * 2];
	unsigned char h3[TNT_SHA1_DIGEST_SIZE];
	unsigned char scramble[TNT_SHA1_DIGEST_SIZE];
	int j;
	char packet_buf[512];
	char *p = packet_buf + 5;
	uint32_t payload_len;
	char resp_hdr[5] = {0};
	uint32_t resp_len;
	char *resp_body = NULL;
	int rc = -1;

	if (!client->config.user[0])
		return 0;

	raw_salt_len = tnt_base64_decode(salt_b64, salt_b64_len, raw_salt, sizeof(raw_salt));
	if (raw_salt_len < TNT_SHA1_DIGEST_SIZE)
		return -1;

	/*
	 * CHAP-SHA1 Scramble:
	 * h1 = SHA1(password)
	 * h2 = SHA1(h1)
	 * h3 = SHA1(salt[0..20] + h2)
	 * scramble = h1 XOR h3
	 */
	tnt_sha1((const unsigned char *)client->config.password,
		 (uint32_t)strlen(client->config.password), h1);
	tnt_sha1(h1, TNT_SHA1_DIGEST_SIZE, h2);

	memcpy(step3_in, raw_salt, TNT_SHA1_DIGEST_SIZE);
	memcpy(step3_in + TNT_SHA1_DIGEST_SIZE, h2, TNT_SHA1_DIGEST_SIZE);
	tnt_sha1(step3_in, TNT_SHA1_DIGEST_SIZE * 2, h3);

	for (j = 0; j < TNT_SHA1_DIGEST_SIZE; j++)
		scramble[j] = h1[j] ^ h3[j];

	/* Header Map(2): { 0x00: IPROTO_AUTH (7), 0x01: sync_id } */
	p = mp_encode_map(p, 2);
	p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
	p = mp_encode_uint(p, IPROTO_AUTH);
	p = mp_encode_uint(p, IPROTO_SYNC);
	p = mp_encode_uint(p, ++client->sync_id);

	/* Body Map(2): { 0x23 (USER): username, 0x21 (TUPLE): ["chap-sha1", bin(20, scramble)] } */
	p = mp_encode_map(p, 2);
	p = mp_encode_uint(p, IPROTO_USER_NAME);
	p = mp_encode_str(p, client->config.user, (uint32_t)strlen(client->config.user));
	p = mp_encode_uint(p, IPROTO_TUPLE);
	p = mp_encode_array(p, 2);
	p = mp_encode_str(p, "chap-sha1", 9);
	p = mp_encode_bin(p, (const char *)scramble, TNT_SHA1_DIGEST_SIZE);

	payload_len = (uint32_t)(p - (packet_buf + 5));
	packet_buf[0] = (char)MP_UINT32;
	packet_buf[1] = (char)(payload_len >> 24);
	packet_buf[2] = (char)(payload_len >> 16);
	packet_buf[3] = (char)(payload_len >> 8);
	packet_buf[4] = (char)(payload_len);

	if (tnt_socket_send_all(client->sock_fd, packet_buf, 5 + payload_len) < 0)
		goto out_cleanup;

	if (tnt_socket_recv_all(client->sock_fd, resp_hdr, 5) < 0 || (uint8_t)resp_hdr[0] != MP_UINT32)
		goto out_cleanup;

	resp_len = ((uint32_t)(uint8_t)resp_hdr[1] << 24) |
		   ((uint32_t)(uint8_t)resp_hdr[2] << 16) |
		   ((uint32_t)(uint8_t)resp_hdr[3] << 8) |
		   ((uint32_t)(uint8_t)resp_hdr[4]);

	if (resp_len == 0 || resp_len > 65536)
		goto out_cleanup;

	resp_body = (char *)calloc(1, resp_len);
	if (!resp_body)
		goto out_cleanup;

	if (tnt_socket_recv_all(client->sock_fd, resp_body, resp_len) < 0)
		goto out_free;

	if ((uint8_t)resp_body[0] >= 0x80 && (uint8_t)resp_body[1] == 0x00) {
		rc = 0;
	}

out_free:
	free(resp_body);
out_cleanup:
	tnt_memzero_explicit(h1, sizeof(h1));
	tnt_memzero_explicit(h2, sizeof(h2));
	tnt_memzero_explicit(h3, sizeof(h3));
	tnt_memzero_explicit(step3_in, sizeof(step3_in));
	tnt_memzero_explicit(scramble, sizeof(scramble));
	tnt_memzero_explicit(raw_salt, sizeof(raw_salt));
	return rc;
}

/**
 * rtpe_tarantool_new_from_config - Allocate and initialize a new client from configuration
 * @cfg: pointer to driver configuration struct
 *
 * Returns allocated client pointer on success, or NULL on failure.
 */
rtpe_tarantool_client_t *rtpe_tarantool_new_from_config(const rtpe_tarantool_config_t *cfg)
{
	rtpe_tarantool_client_t *c;

	if (!cfg)
		return NULL;

	c = (rtpe_tarantool_client_t *)calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	memcpy(&c->config, cfg, sizeof(c->config));
	if (c->config.port <= 0)
		c->config.port = 3301;
	if (c->config.expires_secs <= 0)
		c->config.expires_secs = 3600;
	if (c->config.connect_timeout_ms <= 0)
		c->config.connect_timeout_ms = 500;
	if (c->config.cmd_timeout_ms <= 0)
		c->config.cmd_timeout_ms = 500;
	if (c->config.allowed_errors <= 0)
		c->config.allowed_errors = 3;
	if (c->config.disable_time <= 0)
		c->config.disable_time = 10;
	if (c->config.tcp_keepalive_time <= 0)
		c->config.tcp_keepalive_time = 60;
	if (c->config.tcp_keepalive_intvl <= 0)
		c->config.tcp_keepalive_intvl = 5;
	if (c->config.tcp_keepalive_probes <= 0)
		c->config.tcp_keepalive_probes = 3;
	if (c->config.num_threads <= 0)
		c->config.num_threads = 4;
	if (c->config.space[0] == '\0')
		snprintf(c->config.space, sizeof(c->config.space), "rtpe_calls");

	c->sock_fd = -1;
	c->state = TARANTOOL_DISCONNECTED;
	c->sync_id = 1;
	return c;
}

/**
 * rtpe_tarantool_new - Convenience constructor with explicit connection parameters
 * @host: target Tarantool host or IPv4 address
 * @port: target TCP port
 * @user: username for CHAP-SHA1 authentication
 * @pass: password for CHAP-SHA1 authentication
 * @node_id: RTPEngine instance identifier
 *
 * Returns allocated client instance, or NULL on error.
 */
rtpe_tarantool_client_t *rtpe_tarantool_new(const char *host, int port, const char *user, const char *pass, const char *node_id)
{
	rtpe_tarantool_config_t cfg;

	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.host, sizeof(cfg.host), "%s", host ? host : "127.0.0.1");
	cfg.port = port > 0 ? port : 3301;
	if (user)
		snprintf(cfg.user, sizeof(cfg.user), "%s", user);
	if (pass)
		snprintf(cfg.password, sizeof(cfg.password), "%s", pass);
	snprintf(cfg.node_id, sizeof(cfg.node_id), "%s", node_id ? node_id : "rtpe-default");
	snprintf(cfg.space, sizeof(cfg.space), "rtpe_calls");
	cfg.expires_secs = 3600;
	cfg.connect_timeout_ms = 500;
	cfg.cmd_timeout_ms = 500;
	cfg.allowed_errors = 3;
	cfg.disable_time = 10;
	cfg.num_threads = 4;
	cfg.tcp_keepalive_time = 60;
	cfg.tcp_keepalive_intvl = 5;
	cfg.tcp_keepalive_probes = 3;

	return rtpe_tarantool_new_from_config(&cfg);
}

/**
 * rtpe_tarantool_connect - Open TCP socket, read Greeting handshake, and authenticate
 * @client: Tarantool client instance
 *
 * Returns 0 on success, or -1 on error.
 */
int rtpe_tarantool_connect(rtpe_tarantool_client_t *client)
{
	time_t now;
	struct timeval tv;
	int flag = 1;
	int keepalive = 1;
	struct sockaddr_in serv_addr;
	char greeting[TNT_GREETING_SIZE];
	int rc = -1;

	if (!client)
		return -1;

	now = time(NULL);
	if (client->state == TARANTOOL_DISABLED) {
		if (now < client->disabled_until)
			return -1;
		client->state = TARANTOOL_DISCONNECTED;
		client->consecutive_errors = 0;
	}

	rtpe_tarantool_close(client);

	client->state = TARANTOOL_CONNECTING;
	client->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (client->sock_fd < 0)
		goto out_err;

	/* Configure timeouts */
	tv.tv_sec = client->config.connect_timeout_ms / 1000;
	tv.tv_usec = (suseconds_t)(client->config.connect_timeout_ms % 1000) * 1000;
	setsockopt(client->sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	setsockopt(client->sock_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
	setsockopt(client->sock_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
	setsockopt(client->sock_fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&keepalive, sizeof(int));

#ifdef TCP_KEEPIDLE
	setsockopt(client->sock_fd, IPPROTO_TCP, TCP_KEEPIDLE, &client->config.tcp_keepalive_time, sizeof(int));
#elif defined(TCP_KEEPALIVE)
	setsockopt(client->sock_fd, IPPROTO_TCP, TCP_KEEPALIVE, &client->config.tcp_keepalive_time, sizeof(int));
#endif
#ifdef TCP_KEEPINTVL
	setsockopt(client->sock_fd, IPPROTO_TCP, TCP_KEEPINTVL, &client->config.tcp_keepalive_intvl, sizeof(int));
#endif
#ifdef TCP_KEEPCNT
	setsockopt(client->sock_fd, IPPROTO_TCP, TCP_KEEPCNT, &client->config.tcp_keepalive_probes, sizeof(int));
#endif

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(client->config.port);

	if (inet_pton(AF_INET, client->config.host, &serv_addr.sin_addr) <= 0) {
		struct addrinfo hints, *res = NULL;
		char port_str[16];

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		snprintf(port_str, sizeof(port_str), "%d", client->config.port);

		if (getaddrinfo(client->config.host, port_str, &hints, &res) != 0 || !res)
			goto out_err;

		memcpy(&serv_addr, res->ai_addr, sizeof(serv_addr));
		freeaddrinfo(res);
	}

	if (connect(client->sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
		goto out_err;

	/* Read 128-byte Greeting Handshake */
	if (tnt_socket_recv_all(client->sock_fd, greeting, sizeof(greeting)) < 0)
		goto out_err;

	/* Apply command read/write timeout */
	tv.tv_sec = client->config.cmd_timeout_ms / 1000;
	tv.tv_usec = (suseconds_t)(client->config.cmd_timeout_ms % 1000) * 1000;
	setsockopt(client->sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	setsockopt(client->sock_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

	/* Authenticate if credentials are provided */
	if (client->config.user[0]) {
		if (tnt_auth_scramble(client, greeting + 64, 44) < 0)
			goto out_err;
	}

	client->state = TARANTOOL_AUTHENTICATED;
	client->consecutive_errors = 0;
	return 0;

out_err:
	rtpe_tarantool_close(client);
	client->state = TARANTOOL_ERROR;
	client->total_errors++;
	client->consecutive_errors++;
	if (client->consecutive_errors >= client->config.allowed_errors) {
		client->state = TARANTOOL_DISABLED;
		client->disabled_until = now + client->config.disable_time;
	}
	return rc;
}

/**
 * rtpe_tarantool_pack_call_upsert - Binary serialize rtpe_call_upsert stored procedure
 * @buf: output buffer
 * @buf_size: capacity of output buffer
 * @sync_id: request sequence identifier
 * @node_id: instance identifier
 * @info: media call parameters
 *
 * Returns serialized packet size in bytes, or 0 on error.
 */
size_t rtpe_tarantool_pack_call_upsert(char *buf, size_t buf_size, uint64_t sync_id, const char *node_id, const rtpe_call_info_t *info)
{
	char *p;
	uint32_t payload_len;
	uint32_t cid_len;

	if (!buf || buf_size < 512 || !info || !info->call_id)
		return 0;

	p = buf + 5;
	cid_len = (uint32_t)(info->call_id_len > 0 ? info->call_id_len : strlen(info->call_id));

	/* Header: Map(2) { 0x00: IPROTO_CALL, 0x01: sync_id } */
	p = mp_encode_map(p, 2);
	p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
	p = mp_encode_uint(p, IPROTO_CALL);
	p = mp_encode_uint(p, IPROTO_SYNC);
	p = mp_encode_uint(p, sync_id);

	/* Body: Map(2) { 0x22: "rtpe_call_upsert", 0x21: [call_id, node_id, payload_map, ttl_sec] } */
	p = mp_encode_map(p, 2);
	p = mp_encode_uint(p, IPROTO_FUNCTION_NAME);
	p = mp_encode_str(p, "rtpe_call_upsert", 16);

	p = mp_encode_uint(p, IPROTO_TUPLE);
	p = mp_encode_array(p, 4);
	p = mp_encode_str(p, info->call_id, cid_len);
	p = mp_encode_str(p, node_id ? node_id : "rtpe-01", (uint32_t)strlen(node_id ? node_id : "rtpe-01"));

	/* Media attributes payload map */
	p = mp_encode_map(p, 5);
	p = mp_encode_str(p, "caller_ip", 9);
	p = mp_encode_str(p, info->caller_ip ? info->caller_ip : "0.0.0.0", (uint32_t)strlen(info->caller_ip ? info->caller_ip : "0.0.0.0"));
	p = mp_encode_str(p, "caller_port", 11);
	p = mp_encode_uint(p, (uint64_t)info->caller_port);
	p = mp_encode_str(p, "callee_ip", 9);
	p = mp_encode_str(p, info->callee_ip ? info->callee_ip : "0.0.0.0", (uint32_t)strlen(info->callee_ip ? info->callee_ip : "0.0.0.0"));
	p = mp_encode_str(p, "callee_port", 11);
	p = mp_encode_uint(p, (uint64_t)info->callee_port);
	p = mp_encode_str(p, "srtp_suite", 10);
	p = mp_encode_str(p, info->srtp_suite ? info->srtp_suite : "NONE", (uint32_t)strlen(info->srtp_suite ? info->srtp_suite : "NONE"));

	p = mp_encode_uint(p, (uint64_t)(info->ttl_sec > 0 ? info->ttl_sec : 3600));

	payload_len = (uint32_t)(p - (buf + 5));
	buf[0] = (char)MP_UINT32;
	buf[1] = (char)(payload_len >> 24);
	buf[2] = (char)(payload_len >> 16);
	buf[3] = (char)(payload_len >> 8);
	buf[4] = (char)(payload_len);

	return 5 + payload_len;
}

/**
 * rtpe_tarantool_pack_call_delete - Binary serialize rtpe_call_delete stored procedure
 * @buf: output buffer
 * @buf_size: capacity of output buffer
 * @sync_id: request sequence identifier
 * @call_id: unique Call-ID string
 * @call_id_len: length of Call-ID string
 *
 * Returns serialized packet size in bytes, or 0 on error.
 */
size_t rtpe_tarantool_pack_call_delete(char *buf, size_t buf_size, uint64_t sync_id, const char *call_id, size_t call_id_len)
{
	char *p;
	uint32_t payload_len;
	uint32_t cid_len;

	if (!buf || buf_size < 128 || !call_id)
		return 0;

	p = buf + 5;
	cid_len = (uint32_t)(call_id_len > 0 ? call_id_len : strlen(call_id));

	p = mp_encode_map(p, 2);
	p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
	p = mp_encode_uint(p, IPROTO_CALL);
	p = mp_encode_uint(p, IPROTO_SYNC);
	p = mp_encode_uint(p, sync_id);

	p = mp_encode_map(p, 2);
	p = mp_encode_uint(p, IPROTO_FUNCTION_NAME);
	p = mp_encode_str(p, "rtpe_call_delete", 16);
	p = mp_encode_uint(p, IPROTO_TUPLE);
	p = mp_encode_array(p, 1);
	p = mp_encode_str(p, call_id, cid_len);

	payload_len = (uint32_t)(p - (buf + 5));
	buf[0] = (char)MP_UINT32;
	buf[1] = (char)(payload_len >> 24);
	buf[2] = (char)(payload_len >> 16);
	buf[3] = (char)(payload_len >> 8);
	buf[4] = (char)(payload_len);

	return 5 + payload_len;
}

/**
 * rtpe_tarantool_drain - Discard stale incoming IProto confirmations
 * @client: Tarantool client instance
 */
void rtpe_tarantool_drain(const rtpe_tarantool_client_t *client)
{
	if (!client || client->sock_fd < 0)
		return;
	tnt_socket_drain(client->sock_fd);
}

/**
 * rtpe_tarantool_save_call - Save or refresh an active RTP call session in Tarantool
 * @client: Tarantool client instance
 * @info: media call parameters
 *
 * Returns 0 on success, or negative value on error.
 */
int rtpe_tarantool_save_call(rtpe_tarantool_client_t *client, const rtpe_call_info_t *info)
{
	char packet_buf[2048];
	uint64_t sync;
	uint32_t ttl;
	rtpe_call_info_t call_copy;
	size_t packet_len;

	if (!client || !info)
		return -1;

	sync = client->sync_id++;
	ttl = (info->ttl_sec > 0) ? info->ttl_sec : (uint32_t)client->config.expires_secs;

	memcpy(&call_copy, info, sizeof(call_copy));
	call_copy.ttl_sec = ttl;

	packet_len = rtpe_tarantool_pack_call_upsert(packet_buf, sizeof(packet_buf), sync, client->config.node_id, &call_copy);
	if (packet_len == 0)
		return -1;

	if (client->sock_fd < 0 || client->state != TARANTOOL_AUTHENTICATED) {
		if (rtpe_tarantool_connect(client) != 0)
			return client->config.no_tarantool_required ? 0 : -1;
	}

	tnt_socket_drain(client->sock_fd);

	if (tnt_socket_send_all(client->sock_fd, packet_buf, packet_len) < 0) {
		rtpe_tarantool_close(client);
		client->total_errors++;
		client->consecutive_errors++;
		if (client->consecutive_errors >= client->config.allowed_errors) {
			client->state = TARANTOOL_DISABLED;
			client->disabled_until = time(NULL) + client->config.disable_time;
		}
		return client->config.no_tarantool_required ? 0 : -1;
	}

	client->total_sent++;
	client->consecutive_errors = 0;
	return 0;
}

/**
 * rtpe_tarantool_delete_call - Remove an active call session from Tarantool
 * @client: Tarantool client instance
 * @call_id: unique Call-ID string
 * @call_id_len: length of Call-ID
 *
 * Returns 0 on success, or negative value on error.
 */
int rtpe_tarantool_delete_call(rtpe_tarantool_client_t *client, const char *call_id, size_t call_id_len)
{
	char packet_buf[1024];
	uint64_t sync;
	size_t packet_len;

	if (!client || !call_id)
		return -1;

	sync = client->sync_id++;
	packet_len = rtpe_tarantool_pack_call_delete(packet_buf, sizeof(packet_buf), sync, call_id, call_id_len);
	if (packet_len == 0)
		return -1;

	if (client->sock_fd < 0 || client->state != TARANTOOL_AUTHENTICATED) {
		if (rtpe_tarantool_connect(client) != 0)
			return client->config.no_tarantool_required ? 0 : -1;
	}

	tnt_socket_drain(client->sock_fd);

	if (tnt_socket_send_all(client->sock_fd, packet_buf, packet_len) < 0) {
		rtpe_tarantool_close(client);
		client->total_errors++;
		client->consecutive_errors++;
		if (client->consecutive_errors >= client->config.allowed_errors) {
			client->state = TARANTOOL_DISABLED;
			client->disabled_until = time(NULL) + client->config.disable_time;
		}
		return client->config.no_tarantool_required ? 0 : -1;
	}

	client->total_sent++;
	client->consecutive_errors = 0;
	return 0;
}

/**
 * rtpe_tarantool_restore_calls - Query and restore call legs for failed node with full response unpack
 * @client: Tarantool client instance
 * @node_id: identifier of node to restore
 * @restore_cb: callback invoked for each restored call leg
 * @userdata: opaque context passed to callback
 *
 * Returns number of restored calls on success, or -1 on error.
 */
int rtpe_tarantool_restore_calls(rtpe_tarantool_client_t *client, const char *node_id, rtpe_tarantool_restore_cb_t restore_cb, void *userdata)
{
	char packet[512];
	char *p;
	uint64_t sync;
	uint32_t payload_len;
	char resp_hdr[5] = {0};
	uint32_t resp_len;
	char *resp_body = NULL;
	const char *rptr;
	uint32_t i;
	int restored_count = 0;

	if (!client || !node_id || !restore_cb)
		return -1;

	if (client->sock_fd < 0 || client->state != TARANTOOL_AUTHENTICATED) {
		if (rtpe_tarantool_connect(client) != 0)
			return -1;
	}

	tnt_socket_drain(client->sock_fd);

	p = packet + 5;
	sync = client->sync_id++;

	p = mp_encode_map(p, 2);
	p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
	p = mp_encode_uint(p, IPROTO_CALL);
	p = mp_encode_uint(p, IPROTO_SYNC);
	p = mp_encode_uint(p, sync);

	p = mp_encode_map(p, 2);
	p = mp_encode_uint(p, IPROTO_FUNCTION_NAME);
	p = mp_encode_str(p, "rtpe_call_restore", 17);
	p = mp_encode_uint(p, IPROTO_TUPLE);
	p = mp_encode_array(p, 1);
	p = mp_encode_str(p, node_id, (uint32_t)strlen(node_id));

	payload_len = (uint32_t)(p - (packet + 5));
	packet[0] = (char)MP_UINT32;
	packet[1] = (char)(payload_len >> 24);
	packet[2] = (char)(payload_len >> 16);
	packet[3] = (char)(payload_len >> 8);
	packet[4] = (char)(payload_len);

	if (tnt_socket_send_all(client->sock_fd, packet, 5 + payload_len) < 0) {
		rtpe_tarantool_close(client);
		return -1;
	}

	while (1) {
		uint32_t header_map;
		uint32_t body_map;
		uint64_t msg_sync = 0;

		if (tnt_socket_recv_all(client->sock_fd, resp_hdr, 5) < 0 || (uint8_t)resp_hdr[0] != MP_UINT32) {
			rtpe_tarantool_close(client);
			return -1;
		}

		resp_len = ((uint32_t)(uint8_t)resp_hdr[1] << 24) |
			   ((uint32_t)(uint8_t)resp_hdr[2] << 16) |
			   ((uint32_t)(uint8_t)resp_hdr[3] << 8) |
			   ((uint32_t)(uint8_t)resp_hdr[4]);

		if (resp_len == 0 || resp_len > 1048576) {
			rtpe_tarantool_close(client);
			return -1;
		}

		resp_body = (char *)calloc(1, resp_len);
		if (!resp_body) {
			rtpe_tarantool_close(client);
			return -1;
		}

		if (tnt_socket_recv_all(client->sock_fd, resp_body, resp_len) < 0) {
			free(resp_body);
			rtpe_tarantool_close(client);
			return -1;
		}

		/* Decode 1. Header Map */
		rptr = resp_body;
		header_map = mp_decode_map(&rptr);
		for (i = 0; i < header_map; i++) {
			uint64_t key = mp_decode_uint(&rptr);
			if (key == IPROTO_SYNC) {
				msg_sync = mp_decode_uint(&rptr);
			} else {
				mp_next(&rptr);
			}
		}

		if (msg_sync != sync) {
			free(resp_body);
			resp_body = NULL;
			continue;
		}

		/* Decode 2. Body Map */
		body_map = mp_decode_map(&rptr);
		for (i = 0; i < body_map; i++) {
			uint64_t key = mp_decode_uint(&rptr);
			if (key == IPROTO_DATA) {
				uint32_t data_arr_len = mp_decode_array(&rptr);
				uint32_t tuple_idx;

				for (tuple_idx = 0; tuple_idx < data_arr_len; tuple_idx++) {
					uint8_t item_tag = (uint8_t)*rptr;
					if ((item_tag & 0xf0) == MP_FIXARRAY || item_tag == MP_ARRAY16 || item_tag == MP_ARRAY32) {
						uint32_t sub_count = mp_decode_array(&rptr);
						uint32_t s_idx;
						for (s_idx = 0; s_idx < sub_count; s_idx++) {
							uint8_t sub_tag = (uint8_t)*rptr;
							char cid_buf[128] = {0};
							char nid_buf[64] = {0};
							char c_ip[64] = "0.0.0.0";
							char ce_ip[64] = "0.0.0.0";
							char suite[32] = "NONE";
							int c_port = 0, ce_port = 0;
							rtpe_call_info_t call_info;

							if ((sub_tag & 0xf0) == MP_FIXMAP || sub_tag == MP_MAP16 || sub_tag == MP_MAP32) {
								uint32_t m_size = mp_decode_map(&rptr);
								uint32_t m_i;
								for (m_i = 0; m_i < m_size; m_i++) {
									uint32_t klen = 0;
									const char *kstr = mp_decode_str(&rptr, &klen);
									if (kstr && strncmp(kstr, "call_id", klen) == 0) {
										uint32_t clen = 0;
										const char *c_str = mp_decode_str(&rptr, &clen);
										if (c_str && clen < sizeof(cid_buf)) {
											memcpy(cid_buf, c_str, clen);
											cid_buf[clen] = '\0';
										}
									} else if (kstr && strncmp(kstr, "node_id", klen) == 0) {
										uint32_t nlen = 0;
										const char *n_str = mp_decode_str(&rptr, &nlen);
										if (n_str && nlen < sizeof(nid_buf)) {
											memcpy(nid_buf, n_str, nlen);
											nid_buf[nlen] = '\0';
										}
									} else if (kstr && strncmp(kstr, "payload", klen) == 0 && ((uint8_t)*rptr & 0xf0) == MP_FIXMAP) {
										uint32_t psize = mp_decode_map(&rptr);
										uint32_t pi;
										for (pi = 0; pi < psize; pi++) {
											uint32_t pklen = 0;
											const char *pkstr = mp_decode_str(&rptr, &pklen);
											if (pkstr && strncmp(pkstr, "caller_ip", pklen) == 0) {
												uint32_t vlen = 0;
												const char *vstr = mp_decode_str(&rptr, &vlen);
												if (vstr && vlen < sizeof(c_ip)) {
													memcpy(c_ip, vstr, vlen);
													c_ip[vlen] = '\0';
												}
											} else if (pkstr && strncmp(pkstr, "callee_ip", pklen) == 0) {
												uint32_t vlen = 0;
												const char *vstr = mp_decode_str(&rptr, &vlen);
												if (vstr && vlen < sizeof(ce_ip)) {
													memcpy(ce_ip, vstr, vlen);
													ce_ip[vlen] = '\0';
												}
											} else if (pkstr && strncmp(pkstr, "caller_port", pklen) == 0) {
												c_port = (int)mp_decode_uint(&rptr);
											} else if (pkstr && strncmp(pkstr, "callee_port", pklen) == 0) {
												ce_port = (int)mp_decode_uint(&rptr);
											} else if (pkstr && strncmp(pkstr, "srtp_suite", pklen) == 0) {
												uint32_t vlen = 0;
												const char *vstr = mp_decode_str(&rptr, &vlen);
												if (vstr && vlen < sizeof(suite)) {
													memcpy(suite, vstr, vlen);
													suite[vlen] = '\0';
												}
											} else {
												mp_next(&rptr);
											}
										}
									} else {
										mp_next(&rptr);
									}
								}
							} else {
								mp_next(&rptr);
							}

							if (cid_buf[0] != '\0') {
								memset(&call_info, 0, sizeof(call_info));
								call_info.call_id = cid_buf;
								call_info.call_id_len = strlen(cid_buf);
								call_info.node_id = nid_buf;
								call_info.caller_ip = c_ip;
								call_info.caller_port = c_port;
								call_info.callee_ip = ce_ip;
								call_info.callee_port = ce_port;
								call_info.srtp_suite = suite;

								restored_count++;
								if (restore_cb(&call_info, userdata) != 0)
									break;
							}
						}
					} else {
						mp_next(&rptr);
					}
				}
			} else {
				mp_next(&rptr);
			}
		}
		break;
	}

	free(resp_body);
	return restored_count;
}

/**
 * rtpe_tarantool_close - Close socket and reset connection state
 * @client: Tarantool client instance
 */
void rtpe_tarantool_close(rtpe_tarantool_client_t *client)
{
	if (!client)
		return;

	if (client->sock_fd >= 0) {
		close(client->sock_fd);
		client->sock_fd = -1;
	}
	client->state = TARANTOOL_DISCONNECTED;
}

/**
 * rtpe_tarantool_free - Teardown connection and release client heap memory
 * @client: Tarantool client instance
 */
void rtpe_tarantool_free(rtpe_tarantool_client_t *client)
{
	if (!client)
		return;

	rtpe_tarantool_close(client);
	free(client);
}
