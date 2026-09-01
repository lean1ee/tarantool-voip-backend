/*
 * Copyright (C) 2026 Andrei Lashchinskii <koorwork+kamailio@gmail.com>
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <string.h>

#include "../../core/dprint.h"
#include "../../core/mem/mem.h"
#include "tarantool_client.h"
#include "tarantool_kemi.h"

/**
 * sr_kemi_tarantool_call - Call stored procedure on default Tarantool server
 * @msg: SIP message pointer
 * @proc_name: Procedure name string
 * @params_json: Optional parameters JSON string
 * @res_dst: Destination string for result
 */
int sr_kemi_tarantool_call(
		sip_msg_t *msg, str *proc_name, str *params_json, str *res_dst)
{
	tnt_server_t *srv = NULL;
	(void)msg;

	if(!proc_name || !proc_name->s || proc_name->len <= 0) {
		LM_ERR("invalid procedure name argument\n");
		return -1;
	}

	srv = tnt_get_server(NULL);
	if(!srv) {
		LM_ERR("no default tarantool server available\n");
		return -1;
	}

	return tnt_exec_call(srv, proc_name, params_json, res_dst);
}

/**
 * sr_kemi_tarantool_eval - Evaluate Lua code on default Tarantool server
 * @msg: SIP message pointer
 * @lua_code: Lua code expression string
 * @params_json: Optional parameters JSON string
 * @res_dst: Destination string for result
 */
int sr_kemi_tarantool_eval(
		sip_msg_t *msg, str *lua_code, str *params_json, str *res_dst)
{
	tnt_server_t *srv = NULL;
	(void)msg;

	if(!lua_code || !lua_code->s || lua_code->len <= 0) {
		LM_ERR("invalid lua code expression argument\n");
		return -1;
	}

	srv = tnt_get_server(NULL);
	if(!srv) {
		LM_ERR("no default tarantool server available\n");
		return -1;
	}

	return tnt_exec_eval(srv, lua_code, params_json, res_dst);
}

/**
 * sr_kemi_tarantool_call_srv - Call stored procedure on a named Tarantool server
 * @msg: SIP message pointer
 * @srv_name: Server alias name
 * @proc_name: Procedure name string
 * @params_json: Optional parameters JSON string
 * @res_dst: Destination string for result
 */
int sr_kemi_tarantool_call_srv(sip_msg_t *msg, str *srv_name, str *proc_name,
		str *params_json, str *res_dst)
{
	tnt_server_t *srv = NULL;
	(void)msg;

	if(!proc_name || !proc_name->s || proc_name->len <= 0) {
		LM_ERR("invalid procedure name argument\n");
		return -1;
	}

	srv = tnt_get_server(srv_name);
	if(!srv) {
		LM_ERR("tarantool server '%.*s' not found\n",
				srv_name ? srv_name->len : 0, srv_name ? srv_name->s : "");
		return -1;
	}

	return tnt_exec_call(srv, proc_name, params_json, res_dst);
}

/**
 * sr_kemi_tarantool_eval_srv - Evaluate Lua code on a named Tarantool server
 * @msg: SIP message pointer
 * @srv_name: Server alias name
 * @lua_code: Lua code expression string
 * @params_json: Optional parameters JSON string
 * @res_dst: Destination string for result
 */
int sr_kemi_tarantool_eval_srv(sip_msg_t *msg, str *srv_name, str *lua_code,
		str *params_json, str *res_dst)
{
	tnt_server_t *srv = NULL;
	(void)msg;

	if(!lua_code || !lua_code->s || lua_code->len <= 0) {
		LM_ERR("invalid lua code expression argument\n");
		return -1;
	}

	srv = tnt_get_server(srv_name);
	if(!srv) {
		LM_ERR("tarantool server '%.*s' not found\n",
				srv_name ? srv_name->len : 0, srv_name ? srv_name->s : "");
		return -1;
	}

	return tnt_exec_eval(srv, lua_code, params_json, res_dst);
}

/* KEMI Export Definitions */
static sr_kemi_t sr_kemi_ndb_tarantool_exports[] = {
		{str_init("tarantool"), str_init("call"), SR_KEMIP_INT,
				sr_kemi_tarantool_call,
				{SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE,
						SR_KEMIP_NONE, SR_KEMIP_NONE}},
		{str_init("tarantool"), str_init("eval"), SR_KEMIP_INT,
				sr_kemi_tarantool_eval,
				{SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE,
						SR_KEMIP_NONE, SR_KEMIP_NONE}},
		{str_init("tarantool"), str_init("call_srv"), SR_KEMIP_INT,
				sr_kemi_tarantool_call_srv,
				{SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR,
						SR_KEMIP_NONE, SR_KEMIP_NONE}},
		{str_init("tarantool"), str_init("eval_srv"), SR_KEMIP_INT,
				sr_kemi_tarantool_eval_srv,
				{SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_STR,
						SR_KEMIP_NONE, SR_KEMIP_NONE}},

		{{0, 0}, {0, 0}, 0, NULL, {0, 0, 0, 0, 0, 0}}};

/**
 * sr_kemi_ndb_tarantool_register - Register KEMI exports
 */
int sr_kemi_ndb_tarantool_register(void)
{
	sr_kemi_modules_add(sr_kemi_ndb_tarantool_exports);
	return 0;
}
