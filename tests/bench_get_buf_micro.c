/*
 * Micro-Benchmark: Traditional get() vs Zero-Allocation get_buf()
 * Measures nanosecond lookup overhead, memory allocation churn, and throughput
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include "../opensips_tarantool/cachedb_tarantool/msgpuck.h"

#define IPROTO_REQUEST_TYPE 0x00
#define IPROTO_SYNC         0x01
#define IPROTO_DATA         0x30
#define IPROTO_OK           0x00

typedef struct {
    char *s;
    int len;
} str_t;

/* Simulate traditional get() with heap allocation */
static int bench_traditional_get(const char *body, size_t len, uint64_t want_sync, str_t *out_val) {
    const char *p = body;
    const char *end = body + len;

    uint32_t h_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < h_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        uint64_t v = mp_decode_uint(&p);
        if (k == IPROTO_REQUEST_TYPE && v != IPROTO_OK) return -1;
        if (k == IPROTO_SYNC && v != want_sync) return -2;
    }

    if (p >= end) return -1;
    uint32_t b_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < b_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        if (k == IPROTO_DATA) {
            uint32_t tuple_count = mp_decode_array(&p);
            if (tuple_count == 0) return -2;
            uint32_t field_count = mp_decode_array(&p);
            if (field_count < 1) return -2;

            uint32_t dummy_len = 0;
            mp_decode_str(&p, &dummy_len); // field 0

            if (field_count >= 7) {
                mp_decode_str(&p, &dummy_len); // field 1
                mp_decode_str(&p, &dummy_len); // field 2
                mp_decode_uint(&p);            // field 3
                mp_decode_uint(&p);            // field 4
                mp_decode_uint(&p);            // field 5
                uint32_t val_len = 0;
                const char *val_ptr = mp_decode_str(&p, &val_len); // field 6
                if (val_ptr && val_len > 0) {
                    char *ret_str = (char *)malloc(val_len + 1);
                    if (!ret_str) return -1;
                    memcpy(ret_str, val_ptr, val_len);
                    ret_str[val_len] = '\0';
                    out_val->s = ret_str;
                    out_val->len = (int)val_len;
                    return 0;
                }
            }
        }
    }
    return -2;
}

/* Simulate new get_buf() zero-allocation path */
static int bench_zero_alloc_get_buf(const char *body, size_t len, uint64_t want_sync,
                                    char *dst_buf, unsigned int buflen,
                                    unsigned int *vlen, unsigned int *needed) {
    const char *p = body;
    const char *end = body + len;

    uint32_t h_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < h_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        uint64_t v = mp_decode_uint(&p);
        if (k == IPROTO_REQUEST_TYPE && v != IPROTO_OK) return -1;
        if (k == IPROTO_SYNC && v != want_sync) return -2;
    }

    if (p >= end) return -1;
    uint32_t b_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < b_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        if (k == IPROTO_DATA) {
            uint32_t tuple_count = mp_decode_array(&p);
            if (tuple_count == 0) return -2;
            uint32_t field_count = mp_decode_array(&p);
            if (field_count < 1) return -2;

            uint32_t dummy_len = 0;
            mp_decode_str(&p, &dummy_len); // field 0

            if (field_count >= 7) {
                mp_decode_str(&p, &dummy_len); // field 1
                mp_decode_str(&p, &dummy_len); // field 2
                mp_decode_uint(&p);            // field 3
                mp_decode_uint(&p);            // field 4
                mp_decode_uint(&p);            // field 5
                uint32_t val_len = 0;
                const char *val_ptr = mp_decode_str(&p, &val_len); // field 6
                if (val_ptr && val_len > 0) {
                    if (val_len > buflen) {
                        if (needed) *needed = val_len;
                        return -3;
                    }
                    memcpy(dst_buf, val_ptr, val_len);
                    if (vlen) *vlen = val_len;
                    return 0;
                }
            }
        }
    }
    return -2;
}

static char sample_packet[1024];
static size_t sample_packet_len = 0;

static void prepare_sample_packet(void) {
    char *p = sample_packet;
    
    // Header map
    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
    p = mp_encode_uint(p, IPROTO_OK);
    p = mp_encode_uint(p, IPROTO_SYNC);
    p = mp_encode_uint(p, 100);

    // Body map
    p = mp_encode_map(p, 1);
    p = mp_encode_uint(p, IPROTO_DATA);
    p = mp_encode_array(p, 1); // 1 tuple
    p = mp_encode_array(p, 7); // 7 fields
    
    const char *call_id = "sip-call-987654321@10.0.0.1";
    p = mp_encode_str(p, call_id, strlen(call_id));
    p = mp_encode_str(p, "rtpe_node1", 10);
    p = mp_encode_str(p, "active", 6);
    p = mp_encode_uint(p, 1724500000);
    p = mp_encode_uint(p, 1724500001);
    p = mp_encode_uint(p, 1724503600);
    
    const char *payload = "{\"src_ip\":\"192.168.1.10\",\"dst_ip\":\"192.168.1.20\",\"src_port\":30002,\"dst_port\":30004,\"codec\":\"PCMU/8000\"}";
    p = mp_encode_str(p, payload, strlen(payload));

    sample_packet_len = (size_t)(p - sample_packet);
}

typedef struct {
    int iterations;
    int mode; // 0 = traditional, 1 = get_buf
    double ops_sec;
    double ns_per_op;
} worker_arg_t;

static void *worker_thread(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    struct timespec start, end;
    
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (w->mode == 0) {
        str_t val = {0};
        for (int i = 0; i < w->iterations; i++) {
            val.s = NULL;
            val.len = 0;
            bench_traditional_get(sample_packet, sample_packet_len, 100, &val);
            if (val.s) {
                free(val.s);
            }
        }
    } else {
        char local_buf[512];
        unsigned int vlen = 0, needed = 0;
        for (int i = 0; i < w->iterations; i++) {
            bench_zero_alloc_get_buf(sample_packet, sample_packet_len, 100, local_buf, sizeof(local_buf), &vlen, &needed);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    w->ops_sec = w->iterations / elapsed;
    w->ns_per_op = (elapsed * 1e9) / w->iterations;

    return NULL;
}

static void run_bench(int num_threads, int total_ops) {
    int per_thread = total_ops / num_threads;
    pthread_t threads[16];
    worker_arg_t args[16];

    /* Test Mode 0: Traditional get() with malloc/free */
    for (int i = 0; i < num_threads; i++) {
        args[i].iterations = per_thread;
        args[i].mode = 0;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }
    double total_ops_0 = 0, avg_ns_0 = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_ops_0 += args[i].ops_sec;
        avg_ns_0 += args[i].ns_per_op;
    }
    avg_ns_0 /= num_threads;

    /* Test Mode 1: New get_buf() Zero-Allocation */
    for (int i = 0; i < num_threads; i++) {
        args[i].iterations = per_thread;
        args[i].mode = 1;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }
    double total_ops_1 = 0, avg_ns_1 = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_ops_1 += args[i].ops_sec;
        avg_ns_1 += args[i].ns_per_op;
    }
    avg_ns_1 /= num_threads;

    printf("  %2d threads | get() (malloc): %8.2f Mops/s (%5.1f ns/op) | get_buf() (zero-alloc): %8.2f Mops/s (%5.1f ns/op) | Speedup: %4.2fx\n",
           num_threads,
           total_ops_0 / 1e6, avg_ns_0,
           total_ops_1 / 1e6, avg_ns_1,
           total_ops_1 / total_ops_0);
}

int main(int argc, char **argv) {
    int total_ops = 2000000;
    if (argc > 1) {
        total_ops = atoi(argv[1]);
    }

    prepare_sample_packet();

    printf("====================================================================================================\n");
    printf("     MICRO-BENCHMARK: TRADITIONAL get() VS ZERO-ALLOCATION get_buf() (CACHEDB_CAP_GET_BUF)          \n");
    printf("====================================================================================================\n");
    printf("Payload size: %zu bytes | Total operations per run: %d\n\n", sample_packet_len, total_ops);

    run_bench(1, total_ops);
    run_bench(2, total_ops);
    run_bench(4, total_ops);
    run_bench(8, total_ops);

    printf("====================================================================================================\n");
    return 0;
}
