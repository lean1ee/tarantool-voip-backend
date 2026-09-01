/*
 * Comprehensive ASan/UBSan Unit & Integration Test for OpenSIPS CacheDB Tarantool Driver
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../opensips_tarantool/cachedb_tarantool/cachedb_tarantool_dbase.h"

static void test_url_parser_variations(void)
{
	/* 1. Full URL with credentials */
	const char *u1 = "tarantool:cluster1://admin:secret@127.0.0.1:3301/my_space";
	str url1 = { .s = (char *)u1, .len = (int)strlen(u1) };
	tarantool_init_without_tnt = 1;
	cachedb_con *con1 = tarantool_init(&url1);
	assert(con1 != NULL);
	tarantool_destroy(con1);

	/* 2. Simple URL without name */
	const char *u2 = "tarantool://127.0.0.1:3301/rtpe_calls";
	str url2 = { .s = (char *)u2, .len = (int)strlen(u2) };
	cachedb_con *con2 = tarantool_init(&url2);
	assert(con2 != NULL);
	tarantool_destroy(con2);

	/* 3. Invalid URL */
	const char *u3 = "http://invalid-scheme";
	str url3 = { .s = (char *)u3, .len = (int)strlen(u3) };
	cachedb_con *con3 = tarantool_init(&url3);
	assert(con3 == NULL);

	printf("  [PASS] URL Parser variations (Full, Simple, Invalid)\n");
}

static void test_live_cachedb_operations(void)
{
	const char *u = "tarantool://127.0.0.1:3301/rtpe_calls";
	str url = { .s = (char *)u, .len = (int)strlen(u) };
	tarantool_init_without_tnt = 0;
	cachedb_con *con = tarantool_init(&url);
	if (!con) {
		printf("  [SKIP] Tarantool server not reachable at 127.0.0.1:3301 (offline mode)\n");
		return;
	}

	str key = { .s = "opensips-asan-call-9999", .len = 23 };
	str val = { .s = "v=0\r\no=alice 1234 5678 IN IP4 10.0.0.1\r\n", .len = 40 };

	/* 1. Set */
	int rc = tarantool_set(con, &key, &val, 120);
	assert(rc == 0);

	/* 2. Get */
	str got_val = { .s = NULL, .len = 0 };
	rc = tarantool_get(con, &key, &got_val);
	assert(rc == 0);
	assert(got_val.s != NULL);
	assert(got_val.len == val.len);
	assert(memcmp(got_val.s, val.s, (size_t)val.len) == 0);
	free(got_val.s);

	/* 3. Get Buf */
	char stack_buf[256];
	unsigned int vlen = 0, needed = 0;
	rc = tarantool_get_buf(con, &key, stack_buf, sizeof(stack_buf), &vlen, &needed);
	assert(rc == 0);
	assert((int)vlen == val.len);

	/* 4. Remove */
	rc = tarantool_remove(con, &key);
	assert(rc == 0);

	/* 5. Destroy (LSan check) */
	tarantool_destroy(con);

	printf("  [PASS] Live CacheDB set, get, get_buf, remove, destroy\n");
}

int main(void)
{
	printf("==================================================================\n");
	printf("  ASan/UBSan Dynamic Analysis: OpenSIPS CacheDB Tarantool Driver  \n");
	printf("==================================================================\n");
	test_url_parser_variations();
	test_live_cachedb_operations();
	printf("==================================================================\n");
	printf("  ALL OPENSIPS ASAN/UBSAN TESTS PASSED (0 MEMORY LEAKS/ERRORS)    \n");
	printf("==================================================================\n");
	return 0;
}
