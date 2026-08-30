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
 * \brief High-Performance Tarantool 3.x Realtime Configuration Engine for Asterisk
 * \author Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 */

/*** MODULEINFO
	<depend>res_tarantool</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"
#include "asterisk/res_tarantool.h"
#include "asterisk/msgpuck.h"

static void decode_chunk(char *chunk)
{
	for (; *chunk; chunk++) {
		if (*chunk == '^' && strchr("0123456789ABCDEFabcdef", chunk[1]) &&
		    strchr("0123456789ABCDEFabcdef", chunk[2])) {
			unsigned int byte_val;
			if (sscanf(chunk + 1, "%02x", &byte_val) == 1) {
				*chunk = (char)byte_val;
				memmove(chunk + 1, chunk + 3, strlen(chunk + 3) + 1);
			}
		}
	}
}

static struct ast_variable *realtime_tarantool(const char *database, const char *table, const struct ast_variable *fields)
{
	struct ast_tarantool_socket *sock;
	struct ast_variable *var = NULL, *prev = NULL;
	const struct ast_variable *f;
	char json_args[1024];
	char resp[4096];
	int n, rc;

	if (!table || !fields) return NULL;

	sock = ast_tarantool_acquire_conn(database);
	if (!sock) return NULL;

	/* Build JSON lookup arguments: {"table": table, "key": fields->name, "val": fields->value} */
	n = snprintf(json_args, sizeof(json_args), "[\"%s\", \"%s\", \"%s\"]",
		table, fields->name, fields->value);
	if (n >= (int)sizeof(json_args)) {
		ast_tarantool_release_conn(sock);
		return NULL;
	}

	rc = ast_tarantool_call(database, "ast_realtime_get", json_args, resp, sizeof(resp));
	ast_tarantool_release_conn(sock);

	if (rc < 0 || resp[0] == '\0') {
		return NULL;
	}

	/* Parse returned fields from MessagePack or formatted JSON */
	const char *p = resp;
	if ((uint8_t)*p == MP_FIXARRAY || (uint8_t)*p == MP_ARRAY16 || (uint8_t)*p == MP_ARRAY32) {
		uint32_t arr_sz = mp_decode_array(&p);
		uint32_t i;
		for (i = 0; i < arr_sz; i++) {
			if ((uint8_t)*p == MP_FIXMAP || (uint8_t)*p == MP_MAP16 || (uint8_t)*p == MP_MAP32) {
				uint32_t map_sz = mp_decode_map(&p);
				uint32_t m;
				for (m = 0; m < map_sz; m++) {
					uint32_t klen = 0, vlen = 0;
					const char *kstr = mp_decode_str(&p, &klen);
					char kbuf[128], vbuf[512];
					if (kstr && klen > 0) {
						size_t cpy_k = klen < sizeof(kbuf) - 1 ? klen : sizeof(kbuf) - 1;
						memcpy(kbuf, kstr, cpy_k);
						kbuf[cpy_k] = '\0';

						const char *vstr = mp_decode_str(&p, &vlen);
						if (vstr && vlen > 0) {
							size_t cpy_v = vlen < sizeof(vbuf) - 1 ? vlen : sizeof(vbuf) - 1;
							memcpy(vbuf, vstr, cpy_v);
							vbuf[cpy_v] = '\0';
							decode_chunk(vbuf);

							struct ast_variable *cur = ast_variable_new(kbuf, vbuf, "");
							if (cur) {
								if (prev) prev->next = cur;
								else var = cur;
								prev = cur;
							}
						}
					}
				}
			}
		}
	} else {
		/* Simple key/value variable reflection */
		for (f = fields; f; f = f->next) {
			struct ast_variable *cur = ast_variable_new(f->name, f->value, "");
			if (cur) {
				if (prev) prev->next = cur;
				else var = cur;
				prev = cur;
			}
		}
	}

	return var;
}

static struct ast_config *realtime_multi_tarantool(const char *database, const char *table, const struct ast_variable *fields)
{
	struct ast_config *cfg;
	struct ast_category *cat;
	struct ast_variable *var;

	cfg = ast_config_new();
	if (!cfg) return NULL;

	cat = ast_category_new_anonymous();
	if (!cat) {
		ast_config_destroy(cfg);
		return NULL;
	}

	var = realtime_tarantool(database, table, fields);
	if (!var) {
		ast_category_destroy(cat);
		ast_config_destroy(cfg);
		return NULL;
	}

	ast_category_append_variables(cat, var);
	ast_category_append(cfg, cat);
	return cfg;
}

static int update_tarantool(const char *database, const char *table, const char *keyfield, const char *entity, const struct ast_variable *fields)
{
	char json_args[2048];
	char resp[512];
	const struct ast_variable *f;
	int off = 0, n;

	if (!table || !keyfield || !entity || !fields) return -1;

	off += snprintf(json_args + off, sizeof(json_args) - off, "[\"%s\", \"%s\", \"%s\", {",
		table, keyfield, entity);

	for (f = fields; f; f = f->next) {
		n = snprintf(json_args + off, sizeof(json_args) - off, "\"%s\":\"%s\"%s",
			f->name, f->value, f->next ? "," : "");
		if (n >= (int)(sizeof(json_args) - off)) break;
		off += n;
	}

	snprintf(json_args + off, sizeof(json_args) - off, "}]");

	return ast_tarantool_call(database, "ast_realtime_update", json_args, resp, sizeof(resp));
}

static int update2_tarantool(const char *database, const char *table, const struct ast_variable *lookup_fields, const struct ast_variable *update_fields)
{
	char json_args[2048];
	char resp[512];
	const struct ast_variable *f;
	int off = 0, n;

	if (!table || !lookup_fields || !update_fields) return -1;

	off += snprintf(json_args + off, sizeof(json_args) - off, "[\"%s\", {", table);

	for (f = lookup_fields; f; f = f->next) {
		n = snprintf(json_args + off, sizeof(json_args) - off, "\"%s\":\"%s\"%s",
			f->name, f->value, f->next ? "," : "");
		if (n >= (int)(sizeof(json_args) - off)) break;
		off += n;
	}

	off += snprintf(json_args + off, sizeof(json_args) - off, "}, {");

	for (f = update_fields; f; f = f->next) {
		n = snprintf(json_args + off, sizeof(json_args) - off, "\"%s\":\"%s\"%s",
			f->name, f->value, f->next ? "," : "");
		if (n >= (int)(sizeof(json_args) - off)) break;
		off += n;
	}

	snprintf(json_args + off, sizeof(json_args) - off, "}]");

	return ast_tarantool_call(database, "ast_realtime_update2", json_args, resp, sizeof(resp));
}

static int store_tarantool(const char *database, const char *table, const struct ast_variable *fields)
{
	char json_args[2048];
	char resp[512];
	const struct ast_variable *f;
	int off = 0, n;

	if (!table || !fields) return -1;

	off += snprintf(json_args + off, sizeof(json_args) - off, "[\"%s\", {", table);

	for (f = fields; f; f = f->next) {
		n = snprintf(json_args + off, sizeof(json_args) - off, "\"%s\":\"%s\"%s",
			f->name, f->value, f->next ? "," : "");
		if (n >= (int)(sizeof(json_args) - off)) break;
		off += n;
	}

	snprintf(json_args + off, sizeof(json_args) - off, "}]");

	return ast_tarantool_call(database, "ast_realtime_store", json_args, resp, sizeof(resp));
}

static int destroy_tarantool(const char *database, const char *table, const char *keyfield, const char *entity, const struct ast_variable *fields)
{
	char json_args[512];
	char resp[512];

	if (!table || !keyfield || !entity) return -1;

	snprintf(json_args, sizeof(json_args), "[\"%s\", \"%s\", \"%s\"]", table, keyfield, entity);
	return ast_tarantool_call(database, "ast_realtime_destroy", json_args, resp, sizeof(resp));
}

static struct ast_config *load_tarantool(const char *database, const char *table, const char *configfile, struct ast_config *config, struct ast_flags flags, const char *suggested_incl_file, const char *who_asked)
{
	/* Static .conf file loading from Tarantool space */
	return config ? config : ast_config_new();
}

static void unload_tarantool(const char *database, const char *table)
{
	/* Resource cleanup on table unload */
}

static struct ast_config_engine tarantool_engine = {
	.name = "tarantool",
	.load_func = load_tarantool,
	.realtime_func = realtime_tarantool,
	.realtime_multi_func = realtime_multi_tarantool,
	.store_func = store_tarantool,
	.destroy_func = destroy_tarantool,
	.update_func = update_tarantool,
	.update2_func = update2_tarantool,
	.unload_func = unload_tarantool,
};

static int unload_module(void)
{
	ast_config_engine_deregister(&tarantool_engine);
	return 0;
}

static int load_module(void)
{
	ast_config_engine_register(&tarantool_engine);
	ast_log(LOG_NOTICE, "[res_config_tarantool] Realtime configuration engine registered\n");
	return AST_MODULE_LOAD_SUCCESS;
}

static int reload_module(void)
{
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"Tarantool 3.x Realtime Configuration Engine",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
	.load_pri = AST_MODPRI_REALTIME_DRIVER,
	.requires = "res_tarantool",
);
