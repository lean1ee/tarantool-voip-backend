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
 * \brief High-Performance Tarantool 3.x Dialplan Functions for Asterisk
 * \author Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 */

/*** MODULEINFO
	<depend>res_tarantool</depend>
	<support_level>core</support_level>
 ***/

/*** DOCUMENTATION
	<function name="TARANTOOL" language="en_US">
		<synopsis>
			Execute a stored Lua procedure in Tarantool 3.x in-memory database.
		</synopsis>
		<syntax argsep=",">
			<parameter name="profile" required="true">
				<para>Connection profile defined in <filename>tarantool.conf</filename> (e.g. <literal>tnt1</literal>).</para>
			</parameter>
			<parameter name="procedure" required="true">
				<para>Name of the stored procedure to call in Tarantool.</para>
			</parameter>
			<parameter name="args" required="false">
				<para>Comma-separated arguments passed to the stored procedure.</para>
			</parameter>
		</syntax>
		<description>
			<para>Executes a sub-millisecond stored procedure in Tarantool and returns the result string.</para>
			<example title="Check subscriber balance and rating before dialing">
			same => n,Set(AUTH_RES=${TARANTOOL(tnt1,call_authorize,${CALLERID(num)},${EXTEN})})
			same => n,GotoIf($["${AUTH_RES}" != "OK"]?rejected)
			</example>
		</description>
	</function>
	<function name="TARANTOOL_EVAL" language="en_US">
		<synopsis>
			Evaluate a raw Lua expression in Tarantool 3.x.
		</synopsis>
		<syntax argsep=",">
			<parameter name="profile" required="true">
				<para>Connection profile defined in <filename>tarantool.conf</filename>.</para>
			</parameter>
			<parameter name="expression" required="true">
				<para>Lua expression string to evaluate.</para>
			</parameter>
		</syntax>
		<description>
			<para>Evaluates a Lua expression and returns the result value.</para>
		</description>
	</function>
 ***/

#include "asterisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/app.h"
#include "asterisk/res_tarantool.h"
#include "asterisk/msgpuck.h"

static int function_tarantool_read(struct ast_channel *chan, const char *cmd,
	char *data, char *buf, size_t len)
{
	char *profile, *proc, *args;
	char json_params[2048];
	char resp[4096];
	int rc;

	AST_DECLARE_APP_ARGS(args_info,
		AST_APP_ARG(profile);
		AST_APP_ARG(proc);
		AST_APP_ARG(rest);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "Syntax: TARANTOOL(profile,proc[,args...])\n");
		return -1;
	}

	AST_STANDARD_APP_ARGS(args_info, data);

	profile = args_info.profile;
	proc = args_info.proc;
	args = args_info.rest;

	if (ast_strlen_zero(profile) || ast_strlen_zero(proc)) {
		ast_log(LOG_WARNING, "TARANTOOL requires both profile and proc arguments\n");
		return -1;
	}

	if (!ast_strlen_zero(args)) {
		snprintf(json_params, sizeof(json_params), "[\"%s\"]", args);
	} else {
		strcpy(json_params, "[]");
	}

	rc = ast_tarantool_call(profile, proc, json_params, resp, sizeof(resp));
	if (rc == 0 && resp[0] != '\0') {
		const char *p = resp;
		uint32_t slen = 0;
		if ((uint8_t)*p == MP_FIXARRAY || (uint8_t)*p == MP_ARRAY16) {
			uint32_t arr_sz = mp_decode_array(&p);
			if (arr_sz > 0) {
				const char *s = mp_decode_str(&p, &slen);
				if (s && slen > 0) {
					size_t cpy = slen < len - 1 ? slen : len - 1;
					memcpy(buf, s, cpy);
					buf[cpy] = '\0';
					return 0;
				}
			}
		}
		ast_copy_string(buf, resp, len);
		return 0;
	}

	buf[0] = '\0';
	return rc;
}

static int function_tarantool_eval_read(struct ast_channel *chan, const char *cmd,
	char *data, char *buf, size_t len)
{
	char *profile, *expr;
	char resp[4096];
	int rc;

	AST_DECLARE_APP_ARGS(args_info,
		AST_APP_ARG(profile);
		AST_APP_ARG(expr);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "Syntax: TARANTOOL_EVAL(profile,expression)\n");
		return -1;
	}

	AST_STANDARD_APP_ARGS(args_info, data);

	profile = args_info.profile;
	expr = args_info.expr;

	if (ast_strlen_zero(profile) || ast_strlen_zero(expr)) {
		ast_log(LOG_WARNING, "TARANTOOL_EVAL requires profile and expression\n");
		return -1;
	}

	rc = ast_tarantool_eval(profile, expr, resp, sizeof(resp));
	if (rc == 0) {
		const char *p = resp;
		uint32_t slen = 0;
		if ((uint8_t)*p == MP_FIXARRAY || (uint8_t)*p == MP_ARRAY16) {
			uint32_t arr_sz = mp_decode_array(&p);
			if (arr_sz > 0) {
				const char *s = mp_decode_str(&p, &slen);
				if (s && slen > 0) {
					size_t cpy = slen < len - 1 ? slen : len - 1;
					memcpy(buf, s, cpy);
					buf[cpy] = '\0';
					return 0;
				}
			}
		}
		ast_copy_string(buf, resp, len);
		return 0;
	}

	buf[0] = '\0';
	return rc;
}

static struct ast_custom_function tarantool_function = {
	.name = "TARANTOOL",
	.read = function_tarantool_read,
};

static struct ast_custom_function tarantool_eval_function = {
	.name = "TARANTOOL_EVAL",
	.read = function_tarantool_eval_read,
};

static int unload_module(void)
{
	int res = 0;
	res |= ast_custom_function_unregister(&tarantool_function);
	res |= ast_custom_function_unregister(&tarantool_eval_function);
	return res;
}

static int load_module(void)
{
	int res = 0;
	res |= ast_custom_function_register(&tarantool_function);
	res |= ast_custom_function_register(&tarantool_eval_function);
	return res ? AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT,
	"Tarantool 3.x Dialplan Execution Functions",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.requires = "res_tarantool",
);
