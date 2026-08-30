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
 * \brief Public header for Asterisk Tarantool 3.x resource manager
 * \author Andrei Lashchinskii <koorwork+asterisk@gmail.com>
 */

#ifndef _ASTERISK_RES_TARANTOOL_H
#define _ASTERISK_RES_TARANTOOL_H

#include <stdint.h>
#include <stddef.h>
#include "asterisk/lock.h"
#include "asterisk/linkedlists.h"
#include "asterisk/strings.h"
#include "asterisk/config.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define AST_TNT_DEFAULT_HOST "127.0.0.1"
#define AST_TNT_DEFAULT_PORT 3301
#define AST_TNT_DEFAULT_TIMEOUT_MS 500
#define AST_TNT_DEFAULT_POOL_SIZE 4

/*! \brief Single Tarantool IProto connection descriptor */
struct ast_tarantool_socket {
	int fd;                          /*!< Socket file descriptor */
	int connected;                   /*!< Connection state flag */
	uint64_t sync_id;                /*!< Monotonically increasing IProto sync id */
	ast_mutex_t lock;                /*!< Socket-level lock */
};

/*! \brief Tarantool connection profile / pool */
struct ast_tarantool_profile {
	char name[64];                   /*!< Profile identifier (e.g. 'tnt1') */
	char host[128];                  /*!< Hostname or IPv4/IPv6 */
	int port;                        /*!< Remote IProto port */
	char user[64];                   /*!< Username */
	char secret[64];                 /*!< Password */
	int pool_size;                   /*!< Number of connections */
	int timeout_ms;                  /*!< Network timeout in milliseconds */
	struct ast_tarantool_socket *sockets; /*!< Array of socket connections */
	ast_mutex_t lock;                /*!< Profile lock */
	AST_LIST_ENTRY(ast_tarantool_profile) entry;
};

/*!
 * \brief Acquire a Tarantool connection handle by profile name.
 * \param name Profile name (or NULL for default first profile).
 * \return Pointer to connected socket or NULL on failure.
 */
struct ast_tarantool_socket *ast_tarantool_acquire_conn(const char *name);

/*!
 * \brief Release a previously acquired connection handle.
 * \param sock Pointer to socket.
 */
void ast_tarantool_release_conn(struct ast_tarantool_socket *sock);

/*!
 * \brief Call a stored procedure on Tarantool.
 * \param profile_name Name of the connection profile (or NULL for default).
 * \param proc_name Stored procedure name (e.g., 'call_authorize', 'select_optimal_node').
 * \param params_json JSON or formatted argument string.
 * \param res_buf Buffer to store result string.
 * \param res_size Size of result buffer.
 * \retval 0 on success.
 * \retval -1 on error.
 */
int ast_tarantool_call(const char *profile_name, const char *proc_name,
	const char *params_json, char *res_buf, size_t res_size);

/*!
 * \brief Evaluate a Lua expression on Tarantool.
 * \param profile_name Name of the connection profile (or NULL for default).
 * \param expr Lua expression string.
 * \param res_buf Buffer to store result string.
 * \param res_size Size of result buffer.
 * \retval 0 on success.
 * \retval -1 on error.
 */
int ast_tarantool_eval(const char *profile_name, const char *expr,
	char *res_buf, size_t res_size);

/*!
 * \brief Low-level IProto query helper with MessagePack buffer.
 * \param sock Active socket descriptor.
 * \param req_type IProto opcode (0x01 Select, 0x02 Insert, 0x03 Replace, 0x05 Delete, 0x06 Call, 0x08 Eval).
 * \param body_buf Encoded MessagePack body buffer.
 * \param body_len Length of MessagePack body.
 * \param res_buf Buffer to receive response.
 * \param res_size Size of response buffer.
 * \retval Length of response payload on success, or -1 on error.
 */
int ast_tarantool_exec_raw(struct ast_tarantool_socket *sock, uint32_t req_type,
	const char *body_buf, size_t body_len, char *res_buf, size_t res_size);

#if defined(__cplusplus)
}
#endif

#endif /* _ASTERISK_RES_TARANTOOL_H */
