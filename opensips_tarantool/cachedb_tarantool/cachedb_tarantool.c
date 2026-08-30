/*
 * Copyright (C) 2026 lean1ee <https://github.com/lean1ee>
 *
 * Author: lean1ee
 * Module: cachedb_tarantool - OpenSIPS 3.x CacheDB driver for Tarantool 3.x
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#if __has_include("../../sr_module.h")
#include "../../sr_module.h"
#include "../../dprint.h"
#include "../../error.h"
#include "../../pt.h"
#include "../../cachedb/cachedb.h"
#else
#ifndef STR_H
typedef struct str_t {
    char *s;
    int len;
} str;
#define str_init(str_val) { (str_val), sizeof(str_val) - 1 }
#endif
#ifndef CACHEDB_H
typedef void cachedb_con;
typedef struct cachedb_url {
    str url;
    struct cachedb_url *next;
} cachedb_url;
typedef struct cachedb_funcs {
    cachedb_con *(*init)(str *);
    void (*destroy)(cachedb_con *);
    int (*get)(cachedb_con *, str *, str *);
    int (*set)(cachedb_con *, str *, str *, int);
    int (*remove)(cachedb_con *, str *);
    int (*raw_query)(cachedb_con *, str *, void *, int, int *);
    int capability;
} cachedb_funcs_t;
typedef struct cachedb_engine {
    str name;
    cachedb_funcs_t cdb_func;
} cachedb_engine;
#define register_cachedb(engine) (0)
#define cachedb_store_url(head, url) (0)
#define cachedb_put_connection(name, con) (0)
#define cachedb_get_connection(name, cluster) (NULL)
#define cachedb_free_url(urls) ((void)0)
#define cachedb_end_connections(name) ((void)0)
#endif
#endif

#include "cachedb_tarantool_dbase.h"

static int mod_init(void);
static int child_init(int rank);
static void destroy(void);

static str cache_mod_name = str_init("tarantool");
static struct cachedb_url *tnt_script_urls = NULL;

static int set_connection(unsigned int type, void *val) {
    (void)type;
    return cachedb_store_url(&tnt_script_urls, (char *)val);
}

static const param_export_t params[] = {
    { "cachedb_url",             STR_PARAM | USE_FUNC_PARAM, (void *)&set_connection },
    { "connect_timeout",         INT_PARAM,                  &tarantool_connect_tout },
    { "query_timeout",           INT_PARAM,                  &tarantool_query_tout },
    { "lazy_connect",            INT_PARAM,                  &tarantool_lazy_connect },
    { "disable_time",            INT_PARAM,                  &tarantool_disable_time },
    { "allowed_errors",          INT_PARAM,                  &tarantool_allowed_errors },
    { "pool_size",               INT_PARAM,                  &tarantool_pool_size },
    { "init_without_tarantool",  INT_PARAM,                  &tarantool_init_without_tnt },
    { "tcp_keepalive",           INT_PARAM,                  &tarantool_tcp_keepalive },
    { 0, 0, 0 }
};

struct module_exports exports = {
    "cachedb_tarantool",        /* module name */
    MOD_TYPE_CACHEDB,           /* class of this module */
    MODULE_VERSION,
    DEFAULT_DLFLAGS,            /* dlopen flags */
    0,                          /* load function */
    0,                          /* OpenSIPS module dependencies */
    0,                          /* exported functions */
    0,                          /* exported async functions */
    params,                     /* exported parameters */
    0,                          /* exported statistics */
    0,                          /* exported MI functions */
    0,                          /* exported pseudo-variables */
    0,                          /* exported transformations */
    0,                          /* extra processes */
    0,                          /* module pre-initialization function */
    mod_init,                   /* module initialization function */
    (response_function)0,       /* response handling function */
    (destroy_function)destroy,  /* destroy function */
    child_init,                 /* per-child init function */
    0                           /* reload confirm function */
};

static int mod_init(void) {
    cachedb_engine cde;

    LM_INFO("Initializing module cachedb_tarantool (Tarantool 3.x)...\n");

    memset(&cde, 0, sizeof(cachedb_engine));

    cde.name = cache_mod_name;
    cde.cdb_func.init = tarantool_init;
    cde.cdb_func.destroy = tarantool_destroy;
    cde.cdb_func.get = tarantool_get;
    cde.cdb_func.set = tarantool_set;
    cde.cdb_func.remove = tarantool_remove;
    cde.cdb_func.raw_query = tarantool_raw_query;
    cde.cdb_func.capability = 0;

#ifdef CACHEDB_HAVE_GET_BUF
    cde.cdb_func.get_buf = tarantool_get_buf;
    cde.cdb_func.capability |= CACHEDB_CAP_GET_BUF;
#endif

    if (register_cachedb(&cde) < 0) {
        LM_ERR("Failed to initialize cachedb_tarantool engine\n");
        return -1;
    }

    return 0;
}

static int child_init(int rank) {
    (void)rank;
    struct cachedb_url *it;
    cachedb_con *con;

    for (it = tnt_script_urls; it; it = it->next) {
        con = tarantool_init(&it->url);
        if (con == NULL) {
            LM_ERR("Failed to open connection to Tarantool cluster\n");
            if (!tarantool_init_without_tnt) return -1;
            continue;
        }
        if (cachedb_put_connection(&cache_mod_name, con) < 0) {
            LM_ERR("Failed to insert Tarantool connection into OpenSIPS pool\n");
            return -1;
        }
    }

    cachedb_free_url(tnt_script_urls);
    return 0;
}

static void destroy(void) {
    LM_NOTICE("Destroy module cachedb_tarantool...\n");
    cachedb_end_connections(&cache_mod_name);
}
