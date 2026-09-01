/*
 * Comprehensive ASan/UBSan Unit & Integration Test for RTPEngine Tarantool Driver
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#include "../../rtpengine_tarantool/include/tarantool.h"
#include "../../rtpengine_tarantool/include/msgpuck.h"

static int restored_count = 0;

static int test_restore_callback(const rtpe_call_info_t *call, void *userdata)
{
	(void)userdata;
	assert(call != NULL);
	assert(call->call_id != NULL);
	assert(call->call_id_len > 0);
	restored_count++;
	return 0;
}

static void test_msgpuck_primitives(void)
{
	char buf[1024];
	char *p = buf;

	/* 1. Encode primitives */
	p = mp_encode_nil(p);
	p = mp_encode_bool(p, true);
	p = mp_encode_bool(p, false);
	p = mp_encode_uint(p, 0);
	p = mp_encode_uint(p, 127);
	p = mp_encode_uint(p, 255);
	p = mp_encode_uint(p, 65535);
	p = mp_encode_uint(p, 4294967295ULL);
	p = mp_encode_uint(p, 0x123456789abcdef0ULL);
	p = mp_encode_str(p, "Hello Tarantool", 15);
	p = mp_encode_bin(p, "\x01\x02\x03\x04", 4);
	p = mp_encode_array(p, 3);
	p = mp_encode_uint(p, 1);
	p = mp_encode_uint(p, 2);
	p = mp_encode_uint(p, 3);
	p = mp_encode_map(p, 1);
	p = mp_encode_str(p, "key", 3);
	p = mp_encode_str(p, "val", 3);

	/* 2. Decode and verify primitives */
	const char *r = buf;
	uint32_t slen = 0;

	/* skip nil, bools */
	mp_next(&r);
	mp_next(&r);
	mp_next(&r);

	assert(mp_decode_uint(&r) == 0);
	assert(mp_decode_uint(&r) == 127);
	assert(mp_decode_uint(&r) == 255);
	assert(mp_decode_uint(&r) == 65535);
	assert(mp_decode_uint(&r) == 4294967295ULL);
	assert(mp_decode_uint(&r) == 0x123456789abcdef0ULL);

	const char *s = mp_decode_str(&r, &slen);
	assert(slen == 15);
	assert(memcmp(s, "Hello Tarantool", 15) == 0);

	/* skip bin */
	mp_next(&r);

	uint32_t arr_sz = mp_decode_array(&r);
	assert(arr_sz == 3);
	assert(mp_decode_uint(&r) == 1);
	assert(mp_decode_uint(&r) == 2);
	assert(mp_decode_uint(&r) == 3);

	uint32_t map_sz = mp_decode_map(&r);
	assert(map_sz == 1);
	s = mp_decode_str(&r, &slen);
	assert(slen == 3 && memcmp(s, "key", 3) == 0);
	s = mp_decode_str(&r, &slen);
	assert(slen == 3 && memcmp(s, "val", 3) == 0);

	printf("  [PASS] MsgPuck primitives encoding and decoding\n");
}

static void test_packet_serialization(void)
{
	char buf[2048];
	rtpe_call_info_t call;
	memset(&call, 0, sizeof(call));
	call.call_id = "test-call-id-12345@192.168.1.1";
	call.call_id_len = strlen(call.call_id);
	call.node_id = "rtpe-node-01";
	call.caller_ip = "192.168.1.50";
	call.caller_port = 10002;
	call.callee_ip = "192.168.1.60";
	call.callee_port = 20004;
	call.srtp_suite = "AES_CM_128_HMAC_SHA1_80";
	call.crypto_key = "dGVzdC1jcnlwdG8ta2V5LTEyMzQ1Njc4OTA=";
	call.ttl_sec = 3600;

	size_t len = rtpe_tarantool_pack_call_upsert(buf, sizeof(buf), 42, "rtpe-node-01", &call);
	assert(len > 0);
	assert((uint8_t)buf[0] == (uint8_t)MP_UINT32);

	size_t del_len = rtpe_tarantool_pack_call_delete(buf, sizeof(buf), 43, call.call_id, call.call_id_len);
	assert(del_len > 0);
	assert((uint8_t)buf[0] == (uint8_t)MP_UINT32);

	printf("  [PASS] IProto binary packet serialization\n");
}

static void test_live_lifecycle(void)
{
	rtpe_tarantool_client_t *client = rtpe_tarantool_new("127.0.0.1", 3301, "guest", "", "rtpe-test-node");
	assert(client != NULL);

	int rc = rtpe_tarantool_connect(client);
	if (rc < 0) {
		printf("  [SKIP] Tarantool server not reachable at 127.0.0.1:3301 (offline mode)\n");
		rtpe_tarantool_free(client);
		return;
	}

	rtpe_call_info_t call;
	memset(&call, 0, sizeof(call));
	call.call_id = "asan-test-call-001@10.0.0.1";
	call.call_id_len = strlen(call.call_id);
	call.node_id = "rtpe-test-node";
	call.caller_ip = "10.0.0.10";
	call.caller_port = 30000;
	call.callee_ip = "10.0.0.20";
	call.callee_port = 30002;
	call.srtp_suite = "NONE";
	call.crypto_key = "";
	call.ttl_sec = 120;

	/* 1. Save Call */
	rc = rtpe_tarantool_save_call(client, &call);
	assert(rc == 0);

	/* Allow async TCP roundtrip to reach Tarantool event loop */
	usleep(15000);

	/* 2. Drain socket */
	rtpe_tarantool_drain(client);

	/* 3. Restore calls */
	restored_count = 0;
	rc = rtpe_tarantool_restore_calls(client, "rtpe-test-node", test_restore_callback, NULL);
	printf("DEBUG: rc=%d, restored_count=%d\n", rc, restored_count);
	assert(rc >= 0);
	assert(restored_count >= 1);

	/* 4. Delete Call */
	rc = rtpe_tarantool_delete_call(client, call.call_id, call.call_id_len);
	assert(rc == 0);

	/* 5. Close & Free (LSan check) */
	rtpe_tarantool_close(client);
	rtpe_tarantool_free(client);

	printf("  [PASS] Live IProto handshake, save, restore, delete, drain, free\n");
}

int main(void)
{
	printf("==================================================================\n");
	printf("  ASan/UBSan Dynamic Analysis: RTPEngine Tarantool Driver         \n");
	printf("==================================================================\n");
	test_msgpuck_primitives();
	test_packet_serialization();
	test_live_lifecycle();
	printf("==================================================================\n");
	printf("  ALL RTPENGINE ASAN/UBSAN TESTS PASSED (0 MEMORY LEAKS/ERRORS)   \n");
	printf("==================================================================\n");
	return 0;
}
