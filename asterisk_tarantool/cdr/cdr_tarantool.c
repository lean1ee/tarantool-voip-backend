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
 * \brief High-Performance Non-Blocking Tarantool 3.x CDR Backend
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
#include <time.h>

#include "asterisk/channel.h"
#include "asterisk/cdr.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"
#include "asterisk/res_tarantool.h"
#include "asterisk/msgpuck.h"

#define CONFIG_FILE "cdr_tarantool.conf"
static const char name[] = "tarantool";

static char connection_profile[64] = "tnt1";
static char space_name[64] = "asterisk_cdrs";
static int log_uniqueid = 1;
static ast_mutex_t cdr_lock;

static int tarantool_cdr_log(struct ast_cdr *cdr)
{
	char json_args[2048];
	char resp[256];
	char start_str[32] = "", ans_str[32] = "", end_str[32] = "";
	struct timeval tv;
	int rc;

	if (!cdr) return 0;

	if (cdr->start.tv_sec > 0) {
		tv = cdr->start;
		snprintf(start_str, sizeof(start_str), "%ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);
	}
	if (cdr->answer.tv_sec > 0) {
		tv = cdr->answer;
		snprintf(ans_str, sizeof(ans_str), "%ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);
	}
	if (cdr->end.tv_sec > 0) {
		tv = cdr->end;
		snprintf(end_str, sizeof(end_str), "%ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);
	}

	snprintf(json_args, sizeof(json_args),
		"[\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", %ld, %ld, %d, \"%s\", \"%s\"]",
		log_uniqueid && !ast_strlen_zero(cdr->uniqueid) ? cdr->uniqueid : "",
		S_OR(cdr->accountcode, ""),
		S_OR(cdr->src, ""),
		S_OR(cdr->dst, ""),
		S_OR(cdr->dcontext, ""),
		S_OR(cdr->clid, ""),
		S_OR(cdr->channel, ""),
		S_OR(cdr->dstchannel, ""),
		S_OR(cdr->lastapp, ""),
		S_OR(cdr->lastdata, ""),
		start_str,
		ans_str,
		(long)cdr->duration,
		(long)cdr->billsec,
		(int)cdr->disposition,
		S_OR(cdr->userfield, ""),
		space_name);

	ast_mutex_lock(&cdr_lock);
	rc = ast_tarantool_call(connection_profile, "ast_cdr_save", json_args, resp, sizeof(resp));
	ast_mutex_unlock(&cdr_lock);

	if (rc < 0) {
		ast_debug(1, "[cdr_tarantool] Failed to save CDR uniqueid=%s\n", cdr->uniqueid);
	}
	return 0;
}

static int load_config(int reload)
{
	struct ast_config *cfg;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	const char *val;

	cfg = ast_config_load(CONFIG_FILE, config_flags);
	if (cfg == CONFIG_STATUS_FILEMISSING || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING, "[cdr_tarantool] Configuration file '%s' missing. Using defaults.\n", CONFIG_FILE);
		return 1;
	}

	ast_mutex_lock(&cdr_lock);
	if ((val = ast_variable_retrieve(cfg, "general", "connection"))) {
		ast_copy_string(connection_profile, val, sizeof(connection_profile));
	}
	if ((val = ast_variable_retrieve(cfg, "general", "space"))) {
		ast_copy_string(space_name, val, sizeof(space_name));
	}
	if ((val = ast_variable_retrieve(cfg, "general", "log_uniqueid"))) {
		log_uniqueid = ast_true(val);
	}
	ast_mutex_unlock(&cdr_lock);

	ast_config_destroy(cfg);
	return 1;
}

static int unload_module(void)
{
	ast_cdr_unregister(name);
	ast_mutex_destroy(&cdr_lock);
	return 0;
}

static int load_module(void)
{
	ast_mutex_init(&cdr_lock);
	if (!load_config(0)) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_cdr_register(name, "Tarantool 3.x High-Speed CDR Engine", tarantool_cdr_log)) {
		ast_log(LOG_ERROR, "[cdr_tarantool] Unable to register CDR backend\n");
		ast_mutex_destroy(&cdr_lock);
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_log(LOG_NOTICE, "[cdr_tarantool] Registered high-speed CDR logging to Tarantool 3.x\n");
	return AST_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	return load_config(1) ? 0 : -1;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"Tarantool 3.x In-Memory CDR Backend",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
	.load_pri = AST_MODPRI_CDR_DRIVER,
	.requires = "cdr,res_tarantool",
);
