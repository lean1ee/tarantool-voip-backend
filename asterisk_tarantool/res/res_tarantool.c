/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2026, Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 *
 * Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 * \brief High-Performance Tarantool 3.x Connection Pool and IProto Resource Manager
 * \author Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

/*** DOCUMENTATION
	<manager name="TarantoolStatus" language="en_US">
		<synopsis>
			Show status of Tarantool 3.x connection pools.
		</synopsis>
		<syntax>
			<xi:include xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])" />
		</syntax>
		<description>
			<para>Queries active Tarantool connection profiles and pool states.</para>
		</description>
	</manager>
 ***/

#include "asterisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#include "asterisk/module.h"
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"
#include "asterisk/linkedlists.h"
#include "asterisk/res_tarantool.h"
#include "asterisk/msgpuck.h"

#define IPROTO_OK 0x00
#define IPROTO_SELECT 0x01
#define IPROTO_INSERT 0x02
#define IPROTO_REPLACE 0x03
#define IPROTO_UPDATE 0x04
#define IPROTO_DELETE 0x05
#define IPROTO_CALL 0x06
#define IPROTO_AUTH 0x07
#define IPROTO_EVAL 0x08
#define IPROTO_UPSERT 0x09

#define IPROTO_REQUEST_TYPE 0x00
#define IPROTO_SYNC 0x01
#define IPROTO_SCHEMA_ID 0x05
#define IPROTO_SPACE_ID 0x10
#define IPROTO_INDEX_ID 0x11
#define IPROTO_KEY 0x20
#define IPROTO_TUPLE 0x21
#define IPROTO_FUNCTION_NAME 0x22
#define IPROTO_EXPR 0x27
#define IPROTO_OPS 0x28
#define IPROTO_DATA 0x30
#define IPROTO_ERROR_24 0x31
#define IPROTO_ERROR 0x52

static const char config_file[] = "tarantool.conf";
static AST_LIST_HEAD_STATIC(profiles, ast_tarantool_profile);
static ast_mutex_t profiles_lock;

static int tnt_socket_connect(struct ast_tarantool_profile *prof, struct ast_tarantool_socket *sock)
{
	int fd;
	struct sockaddr_in sin;
	struct timeval tv;
	int flag = 1;
	char greeting[128];
	ssize_t n;

	if (sock->fd >= 0) {
		close(sock->fd);
		sock->fd = -1;
		sock->connected = 0;
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		ast_log(LOG_ERROR, "[res_tarantool] socket() failed: %s\n", strerror(errno));
		return -1;
	}

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));

	tv.tv_sec = prof->timeout_ms / 1000;
	tv.tv_usec = (prof->timeout_ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(prof->port);
	if (inet_pton(AF_INET, prof->host, &sin.sin_addr) <= 0) {
		close(fd);
		ast_log(LOG_ERROR, "[res_tarantool] Invalid host IP: %s\n", prof->host);
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		close(fd);
		ast_debug(1, "[res_tarantool] connect to %s:%d failed: %s\n",
			prof->host, prof->port, strerror(errno));
		return -1;
	}

	n = recv(fd, greeting, sizeof(greeting), MSG_WAITALL);
	if (n != 128 || memcmp(greeting, "Tarantool", 9) != 0) {
		close(fd);
		ast_log(LOG_WARNING, "[res_tarantool] Invalid IProto greeting from %s:%d\n",
			prof->host, prof->port);
		return -1;
	}

	sock->fd = fd;
	sock->connected = 1;
	ast_debug(1, "[res_tarantool] Connected to Tarantool at %s:%d\n", prof->host, prof->port);
	return 0;
}

static struct ast_tarantool_profile *find_profile(const char *name)
{
	struct ast_tarantool_profile *cur;

	AST_LIST_TRAVERSE(&profiles, cur, entry) {
		if (!name || !name[0] || strcasecmp(cur->name, name) == 0) {
			return cur;
		}
	}
	return NULL;
}

struct ast_tarantool_socket *ast_tarantool_acquire_conn(const char *name)
{
	struct ast_tarantool_profile *prof;
	int i;

	ast_mutex_lock(&profiles_lock);
	prof = find_profile(name);
	if (!prof) {
		ast_mutex_unlock(&profiles_lock);
		ast_log(LOG_WARNING, "[res_tarantool] Profile '%s' not found\n", name ? name : "default");
		return NULL;
	}

	ast_mutex_lock(&prof->lock);
	ast_mutex_unlock(&profiles_lock);

	for (i = 0; i < prof->pool_size; i++) {
		struct ast_tarantool_socket *s = &prof->sockets[i];
		if (ast_mutex_trylock(&s->lock) == 0) {
			if (!s->connected || s->fd < 0) {
				if (tnt_socket_connect(prof, s) < 0) {
					ast_mutex_unlock(&s->lock);
					continue;
				}
			}
			ast_mutex_unlock(&prof->lock);
			return s;
		}
	}

	/* Fallback: block on first socket */
	ast_mutex_lock(&prof->sockets[0].lock);
	if (!prof->sockets[0].connected || prof->sockets[0].fd < 0) {
		tnt_socket_connect(prof, &prof->sockets[0]);
	}
	ast_mutex_unlock(&prof->lock);
	return &prof->sockets[0];
}

void ast_tarantool_release_conn(struct ast_tarantool_socket *sock)
{
	if (sock) {
		ast_mutex_unlock(&sock->lock);
	}
}

int ast_tarantool_exec_raw(struct ast_tarantool_socket *sock, uint32_t req_type,
	const char *body_buf, size_t body_len, char *res_buf, size_t res_size)
{
	char hdr_buf[32];
	char *hp = hdr_buf;
	uint64_t sync;
	uint32_t total_len;
	uint8_t fixlen_buf[5];
	struct iovec iov[3];
	int iovcnt = 0;
	ssize_t sent, n;
	uint8_t rlen_hdr[5];
	uint32_t resp_len;
	char static_resp[4096];
	char *rbuf = static_resp;
	uint32_t to_read;
	size_t off = 0;
	const char *p;
	uint32_t map_size, i;
	uint32_t code = 0;

	if (!sock || sock->fd < 0 || !sock->connected) {
		return -1;
	}

	sync = ++sock->sync_id;

	/* IProto header map(2): { 0x00: type, 0x01: sync } */
	hp = mp_encode_map(hp, 2);
	hp = mp_encode_uint(hp, IPROTO_REQUEST_TYPE);
	hp = mp_encode_uint(hp, req_type);
	hp = mp_encode_uint(hp, IPROTO_SYNC);
	hp = mp_encode_uint(hp, sync);

	total_len = (uint32_t)((hp - hdr_buf) + body_len);

	/* 5-byte fixlength: MP_UINT32 tag + 4-byte big-endian */
	fixlen_buf[0] = (uint8_t)MP_UINT32;
	fixlen_buf[1] = (uint8_t)(total_len >> 24);
	fixlen_buf[2] = (uint8_t)(total_len >> 16);
	fixlen_buf[3] = (uint8_t)(total_len >> 8);
	fixlen_buf[4] = (uint8_t)total_len;

	iov[iovcnt].iov_base = fixlen_buf;
	iov[iovcnt].iov_len = 5;
	iovcnt++;

	iov[iovcnt].iov_base = hdr_buf;
	iov[iovcnt].iov_len = hp - hdr_buf;
	iovcnt++;

	if (body_buf && body_len > 0) {
		iov[iovcnt].iov_base = (void *)body_buf;
		iov[iovcnt].iov_len = body_len;
		iovcnt++;
	}

	sent = writev(sock->fd, iov, iovcnt);
	if (sent < (ssize_t)(5 + total_len)) {
		sock->connected = 0;
		close(sock->fd);
		sock->fd = -1;
		return -1;
	}

	/* Receive 5-byte length */
	n = recv(sock->fd, rlen_hdr, 5, MSG_WAITALL);
	if (n != 5 || rlen_hdr[0] != (uint8_t)MP_UINT32) {
		sock->connected = 0;
		close(sock->fd);
		sock->fd = -1;
		return -1;
	}

	resp_len = ((uint32_t)rlen_hdr[1] << 24) |
	           ((uint32_t)rlen_hdr[2] << 16) |
	           ((uint32_t)rlen_hdr[3] << 8)  |
	           (uint32_t)rlen_hdr[4];

	if (resp_len > sizeof(static_resp)) {
		rbuf = ast_malloc(resp_len);
		if (!rbuf) return -1;
	}

	to_read = resp_len;
	while (to_read > 0) {
		n = recv(sock->fd, rbuf + off, to_read, 0);
		if (n <= 0) {
			if (rbuf != static_resp) ast_free(rbuf);
			sock->connected = 0;
			close(sock->fd);
			sock->fd = -1;
			return -1;
		}
		off += n;
		to_read -= n;
	}

	/* Parse response header */
	p = rbuf;
	map_size = mp_decode_map(&p);
	for (i = 0; i < map_size; i++) {
		uint32_t k = mp_decode_uint(&p);
		if (k == 0x00) {
			code = mp_decode_uint(&p);
		} else {
			/* Skip value */
			mp_decode_uint(&p);
		}
	}

	if (code != IPROTO_OK) {
		if (rbuf != static_resp) ast_free(rbuf);
		return -1;
	}

	if (res_buf && res_size > 0) {
		size_t body_sz = resp_len - (p - rbuf);
		size_t cpy_sz = body_sz < res_size - 1 ? body_sz : res_size - 1;
		memcpy(res_buf, p, cpy_sz);
		res_buf[cpy_sz] = '\0';
	}

	if (rbuf != static_resp) ast_free(rbuf);
	return (int)(resp_len - (p - rbuf));
}

int ast_tarantool_call(const char *profile_name, const char *proc_name,
	const char *params_json, char *res_buf, size_t res_size)
{
	struct ast_tarantool_socket *sock;
	char body[2048];
	char *bp = body;
	int rc;

	if (!proc_name) return -1;

	sock = ast_tarantool_acquire_conn(profile_name);
	if (!sock) return -1;

	/* IProto CALL map(2): { 0x22: function_name, 0x21: tuple [args] } */
	bp = mp_encode_map(bp, 2);
	bp = mp_encode_uint(bp, IPROTO_FUNCTION_NAME);
	bp = mp_encode_str(bp, proc_name, strlen(proc_name));
	bp = mp_encode_uint(bp, IPROTO_TUPLE);

	if (params_json && params_json[0]) {
		bp = mp_encode_array(bp, 1);
		bp = mp_encode_str(bp, params_json, strlen(params_json));
	} else {
		bp = mp_encode_array(bp, 0);
	}

	rc = ast_tarantool_exec_raw(sock, IPROTO_CALL, body, bp - body, res_buf, res_size);
	ast_tarantool_release_conn(sock);
	return rc >= 0 ? 0 : -1;
}

int ast_tarantool_eval(const char *profile_name, const char *expr,
	char *res_buf, size_t res_size)
{
	struct ast_tarantool_socket *sock;
	char body[2048];
	char *bp = body;
	int rc;

	if (!expr) return -1;

	sock = ast_tarantool_acquire_conn(profile_name);
	if (!sock) return -1;

	/* IProto EVAL map(2): { 0x27: expr, 0x21: tuple [] } */
	bp = mp_encode_map(bp, 2);
	bp = mp_encode_uint(bp, IPROTO_EXPR);
	bp = mp_encode_str(bp, expr, strlen(expr));
	bp = mp_encode_uint(bp, IPROTO_TUPLE);
	bp = mp_encode_array(bp, 0);

	rc = ast_tarantool_exec_raw(sock, IPROTO_EVAL, body, bp - body, res_buf, res_size);
	ast_tarantool_release_conn(sock);
	return rc >= 0 ? 0 : -1;
}

static char *handle_cli_tarantool_show_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ast_tarantool_profile *p;
	int count = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "tarantool show status";
		e->usage =
			"Usage: tarantool show status\n"
			"       Shows configured Tarantool 3.x connection profiles and pool health.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ast_cli(a->fd, "=== Tarantool 3.x Connection Profiles ===\n");
	ast_mutex_lock(&profiles_lock);
	AST_LIST_TRAVERSE(&profiles, p, entry) {
		int i, active = 0;
		for (i = 0; i < p->pool_size; i++) {
			if (p->sockets[i].connected && p->sockets[i].fd >= 0) active++;
		}
		ast_cli(a->fd, "Profile: [%s] -> %s:%d (User: %s, Pool: %d/%d active, Timeout: %d ms)\n",
			p->name, p->host, p->port, p->user[0] ? p->user : "<none>",
			active, p->pool_size, p->timeout_ms);
		count++;
	}
	ast_mutex_unlock(&profiles_lock);

	ast_cli(a->fd, "Total Profiles Configured: %d\n", count);
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_tarantool[] = {
	AST_CLI_DEFINE(handle_cli_tarantool_show_status, "Show Tarantool 3.x connection pool status"),
};

static int load_config(int reload)
{
	struct ast_config *cfg;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	const char *cat;

	cfg = ast_config_load(config_file, config_flags);
	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING, "[res_tarantool] Unable to load config '%s'. Using defaults.\n", config_file);
		return 0;
	}

	ast_mutex_lock(&profiles_lock);

	for (cat = ast_category_browse(cfg, NULL); cat; cat = ast_category_browse(cfg, cat)) {
		struct ast_variable *var;
		struct ast_tarantool_profile *prof;
		int pool_sz = AST_TNT_DEFAULT_POOL_SIZE;
		int timeout = AST_TNT_DEFAULT_TIMEOUT_MS;
		int port = AST_TNT_DEFAULT_PORT;
		const char *host = AST_TNT_DEFAULT_HOST;
		const char *user = "";
		const char *secret = "";

		if (strcasecmp(cat, "general") == 0) continue;

		for (var = ast_variable_browse(cfg, cat); var; var = var->next) {
			if (strcasecmp(var->name, "host") == 0) host = var->value;
			else if (strcasecmp(var->name, "port") == 0) port = atoi(var->value);
			else if (strcasecmp(var->name, "user") == 0) user = var->value;
			else if (strcasecmp(var->name, "secret") == 0 || strcasecmp(var->name, "password") == 0) secret = var->value;
			else if (strcasecmp(var->name, "pool_size") == 0) pool_sz = atoi(var->value);
			else if (strcasecmp(var->name, "timeout_ms") == 0) timeout = atoi(var->value);
		}

		prof = find_profile(cat);
		if (!prof) {
			int i;
			prof = ast_calloc(1, sizeof(*prof));
			if (!prof) continue;

			ast_copy_string(prof->name, cat, sizeof(prof->name));
			ast_copy_string(prof->host, host, sizeof(prof->host));
			prof->port = port;
			ast_copy_string(prof->user, user, sizeof(prof->user));
			ast_copy_string(prof->secret, secret, sizeof(prof->secret));
			prof->pool_size = pool_sz > 0 ? pool_sz : AST_TNT_DEFAULT_POOL_SIZE;
			prof->timeout_ms = timeout > 0 ? timeout : AST_TNT_DEFAULT_TIMEOUT_MS;

			ast_mutex_init(&prof->lock);
			prof->sockets = ast_calloc(prof->pool_size, sizeof(struct ast_tarantool_socket));
			for (i = 0; i < prof->pool_size; i++) {
				prof->sockets[i].fd = -1;
				prof->sockets[i].connected = 0;
				ast_mutex_init(&prof->sockets[i].lock);
			}

			AST_LIST_INSERT_TAIL(&profiles, prof, entry);
			ast_debug(1, "[res_tarantool] Added profile [%s] -> %s:%d\n", prof->name, prof->host, prof->port);
		}
	}

	ast_mutex_unlock(&profiles_lock);
	ast_config_destroy(cfg);
	return 0;
}

static int unload_module(void)
{
	struct ast_tarantool_profile *prof;

	ast_cli_unregister_multiple(cli_tarantool, ARRAY_LEN(cli_tarantool));

	ast_mutex_lock(&profiles_lock);
	while ((prof = AST_LIST_REMOVE_HEAD(&profiles, entry))) {
		int i;
		for (i = 0; i < prof->pool_size; i++) {
			if (prof->sockets[i].fd >= 0) close(prof->sockets[i].fd);
			ast_mutex_destroy(&prof->sockets[i].lock);
		}
		ast_free(prof->sockets);
		ast_mutex_destroy(&prof->lock);
		ast_free(prof);
	}
	ast_mutex_unlock(&profiles_lock);
	ast_mutex_destroy(&profiles_lock);

	return 0;
}

static int load_module(void)
{
	ast_mutex_init(&profiles_lock);
	load_config(0);
	ast_cli_register_multiple(cli_tarantool, ARRAY_LEN(cli_tarantool));
	ast_log(LOG_NOTICE, "[res_tarantool] Tarantool 3.x resource engine loaded successfully\n");
	return AST_MODULE_LOAD_SUCCESS;
}

static int reload_module(void)
{
	load_config(1);
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS | AST_MODFLAG_LOAD_ORDER,
	"Tarantool 3.x Connection Pool and IProto Resource Engine",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
	.load_pri = AST_MODPRI_REALTIME_DEPEND,
);
