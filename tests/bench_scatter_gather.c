/*
 * tests/bench_scatter_gather.c
 * Standalone High-Performance Benchmark: Linear Buffer vs Scatter-Gather (writev)
 * Zero-Copy & Zero-Allocation Performance Analysis for Tarantool 3.x IProto
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "../opensips_tarantool/cachedb_tarantool/msgpuck.h"

#define IPROTO_REQUEST_TYPE   0x00
#define IPROTO_SYNC           0x01
#define IPROTO_SPACE_ID       0x10
#define IPROTO_TUPLE          0x21
#define IPROTO_REPLACE        0x03
#define IPROTO_OK             0x00

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int connect_tnt(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(nodelay));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Read greeting */
    char greeting[128];
    if (recv(fd, greeting, sizeof(greeting), MSG_WAITALL) != 128) {
        close(fd);
        return -1;
    }

    return fd;
}

/* Linear write: assemble buffer with memcpy and send() */
static int write_linear(int fd, uint64_t sync_id, uint32_t space_id,
                        const char *key, size_t key_len,
                        const char *val, size_t val_len) {
    char sbuf[4096];
    char *buf = sbuf;
    size_t need = 160 + key_len + val_len;
    if (need > sizeof(sbuf)) {
        buf = (char *)malloc(need);
        if (!buf) return -1;
    }

    time_t now = time(NULL);
    uint64_t expires_at = (uint64_t)now + 3600;

    char *p = buf + 5;
    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
    p = mp_encode_uint(p, IPROTO_REPLACE);
    p = mp_encode_uint(p, IPROTO_SYNC);
    p = mp_encode_uint(p, sync_id);

    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_SPACE_ID);
    p = mp_encode_uint(p, space_id);
    p = mp_encode_uint(p, IPROTO_TUPLE);
    p = mp_encode_array(p, 7);

    p = mp_encode_str(p, key, key_len);
    p = mp_encode_str(p, "bench", 5);
    p = mp_encode_str(p, "active", 6);
    p = mp_encode_uint(p, (uint64_t)now);
    p = mp_encode_uint(p, (uint64_t)now);
    p = mp_encode_uint(p, expires_at);
    p = mp_encode_str(p, val, val_len);

    size_t payload_len = (size_t)(p - buf - 5);
    buf[0] = (char)0xce;
    buf[1] = (char)(payload_len >> 24);
    buf[2] = (char)(payload_len >> 16);
    buf[3] = (char)(payload_len >> 8);
    buf[4] = (char)(payload_len);

    size_t total_len = payload_len + 5;
    ssize_t n = send(fd, buf, total_len, 0);

    if (buf != sbuf) free(buf);
    if (n != (ssize_t)total_len) return -1;

    /* Read response header */
    char resp_pfx[5];
    if (recv(fd, resp_pfx, 5, MSG_WAITALL) != 5) return -1;
    uint32_t resp_len = ((uint32_t)(uint8_t)resp_pfx[1] << 24) |
                        ((uint32_t)(uint8_t)resp_pfx[2] << 16) |
                        ((uint32_t)(uint8_t)resp_pfx[3] << 8) |
                        (uint32_t)(uint8_t)resp_pfx[4];

    char drain_buf[1024];
    uint32_t remaining = resp_len;
    while (remaining > 0) {
        size_t to_read = remaining > sizeof(drain_buf) ? sizeof(drain_buf) : remaining;
        ssize_t r = recv(fd, drain_buf, to_read, MSG_WAITALL);
        if (r <= 0) return -1;
        remaining -= (uint32_t)r;
    }

    return 0;
}

/* Scatter-Gather write: writev() with zero memory allocations and zero payload memcpy */
static int write_scatter_gather(int fd, uint64_t sync_id, uint32_t space_id,
                                const char *key, size_t key_len,
                                const char *val, size_t val_len) {
    time_t now = time(NULL);
    uint64_t expires_at = (uint64_t)now + 3600;

    char hdr_buf[64];
    char meta_buf[64];
    char val_hdr[8];

    /* 1. Pre-encode Header & Tuple map */
    char *hp = hdr_buf + 5;
    hp = mp_encode_map(hp, 2);
    hp = mp_encode_uint(hp, IPROTO_REQUEST_TYPE);
    hp = mp_encode_uint(hp, IPROTO_REPLACE);
    hp = mp_encode_uint(hp, IPROTO_SYNC);
    hp = mp_encode_uint(hp, sync_id);
    hp = mp_encode_map(hp, 2);
    hp = mp_encode_uint(hp, IPROTO_SPACE_ID);
    hp = mp_encode_uint(hp, space_id);
    hp = mp_encode_uint(hp, IPROTO_TUPLE);
    hp = mp_encode_array(hp, 7);

    /* Encode Key string prefix */
    if (key_len <= 31) {
        *hp++ = (char)(0xa0 | key_len);
    } else {
        *hp++ = (char)0xd9;
        *hp++ = (char)key_len;
    }
    size_t hdr_payload = (size_t)(hp - hdr_buf - 5);

    /* 2. Encode fixed metadata fields (node_id, state, created_at, updated_at, expires_at) */
    char *mp = meta_buf;
    mp = mp_encode_str(mp, "bench", 5);
    mp = mp_encode_str(mp, "active", 6);
    mp = mp_encode_uint(mp, (uint64_t)now);
    mp = mp_encode_uint(mp, (uint64_t)now);
    mp = mp_encode_uint(mp, expires_at);
    size_t meta_len = (size_t)(mp - meta_buf);

    /* 3. Encode Value string header prefix */
    char *vp = val_hdr;
    if (val_len <= 31) {
        *vp++ = (char)(0xa0 | val_len);
    } else if (val_len <= 0xff) {
        *vp++ = (char)0xd9;
        *vp++ = (char)val_len;
    } else if (val_len <= 0xffff) {
        *vp++ = (char)0xda;
        *vp++ = (char)(val_len >> 8);
        *vp++ = (char)val_len;
    } else {
        *vp++ = (char)0xdb;
        *vp++ = (char)(val_len >> 24);
        *vp++ = (char)(val_len >> 16);
        *vp++ = (char)(val_len >> 8);
        *vp++ = (char)val_len;
    }
    size_t val_hdr_len = (size_t)(vp - val_hdr);

    /* Calculate total payload length across all vectors */
    uint32_t total_payload = (uint32_t)(hdr_payload + key_len + meta_len + val_hdr_len + val_len);
    hdr_buf[0] = (char)0xce;
    hdr_buf[1] = (char)(total_payload >> 24);
    hdr_buf[2] = (char)(total_payload >> 16);
    hdr_buf[3] = (char)(total_payload >> 8);
    hdr_buf[4] = (char)(total_payload);

    /* Set up 5 I/O vectors (Scatter-Gather) */
    struct iovec iov[5];
    iov[0].iov_base = hdr_buf;
    iov[0].iov_len  = hdr_payload + 5;

    iov[1].iov_base = (void *)key;     // Direct pointer to key, ZERO-COPY
    iov[1].iov_len  = key_len;

    iov[2].iov_base = meta_buf;
    iov[2].iov_len  = meta_len;

    iov[3].iov_base = val_hdr;
    iov[3].iov_len  = val_hdr_len;

    iov[4].iov_base = (void *)val;     // Direct pointer to payload SDP, ZERO-COPY
    iov[4].iov_len  = val_len;

    ssize_t total_sent = (ssize_t)(total_payload + 5);
    ssize_t n = writev(fd, iov, 5);
    if (n != total_sent) return -1;

    /* Read response header */
    char resp_pfx[5];
    if (recv(fd, resp_pfx, 5, MSG_WAITALL) != 5) return -1;
    uint32_t resp_len = ((uint32_t)(uint8_t)resp_pfx[1] << 24) |
                        ((uint32_t)(uint8_t)resp_pfx[2] << 16) |
                        ((uint32_t)(uint8_t)resp_pfx[3] << 8) |
                        (uint32_t)(uint8_t)resp_pfx[4];

    char drain_buf[1024];
    uint32_t remaining = resp_len;
    while (remaining > 0) {
        size_t to_read = remaining > sizeof(drain_buf) ? sizeof(drain_buf) : remaining;
        ssize_t r = recv(fd, drain_buf, to_read, MSG_WAITALL);
        if (r <= 0) return -1;
        remaining -= (uint32_t)r;
    }

    return 0;
}

static void run_bench_size(int fd1, int fd2, int iterations, const char *size_label, const char *sdp_sample, size_t sdp_len) {
    printf("\n>>> Testing Payload: %s (%zu bytes) <<<\n", size_label, sdp_len);

    /* Benchmark 1: Linear Buffer (send) */
    uint64_t t0 = get_time_ns();
    for (int i = 0; i < iterations; i++) {
        char key[64];
        snprintf(key, sizeof(key), "lin-%s-%d", size_label, i);
        if (write_linear(fd1, i + 1000, 512, key, strlen(key), sdp_sample, sdp_len) != 0) {
            fprintf(stderr, "[-] Error in write_linear at iter %d\n", i);
            break;
        }
    }
    uint64_t t1 = get_time_ns();
    double linear_sec = (double)(t1 - t0) / 1e9;
    double linear_ops = (double)iterations / linear_sec;
    double linear_lat_us = (linear_sec * 1e6) / (double)iterations;

    /* Benchmark 2: Scatter-Gather (writev) */
    uint64_t t2 = get_time_ns();
    for (int i = 0; i < iterations; i++) {
        char key[64];
        snprintf(key, sizeof(key), "scat-%s-%d", size_label, i);
        if (write_scatter_gather(fd2, i + 1000, 512, key, strlen(key), sdp_sample, sdp_len) != 0) {
            fprintf(stderr, "[-] Error in write_scatter_gather at iter %d\n", i);
            break;
        }
    }
    uint64_t t3 = get_time_ns();
    double scatter_sec = (double)(t3 - t2) / 1e9;
    double scatter_ops = (double)iterations / scatter_sec;
    double scatter_lat_us = (scatter_sec * 1e6) / (double)iterations;

    /* Output Comparison Table */
    printf("%-26s | %-12s | %-12s | %-12s\n", "Method", "Throughput", "Avg Latency", "Heap Alloc");
    printf("------------------------------------------------------------------------\n");
    printf("%-26s | %9.0f ops/s | %9.2f us | %-12s\n",
           "1. Linear (memcpy+send)", linear_ops, linear_lat_us, sdp_len > 3500 ? "malloc/free" : "Stack");
    printf("%-26s | %9.0f ops/s | %9.2f us | %-12s\n",
           "2. Scatter-Gather (writev)", scatter_ops, scatter_lat_us, "0 B (ZERO)");

    double diff_pct = ((scatter_ops - linear_ops) / linear_ops) * 100.0;
    printf("[+] Delta: %+.2f%% throughput | Zero-Copy: YES\n", diff_pct);
}

int main(int argc, char **argv) {
    int iterations = 10000;
    if (argc > 1) iterations = atoi(argv[1]);

    const char *host = "127.0.0.1";
    int port = 3301;
    if (argc > 2) host = argv[2];
    if (argc > 3) port = atoi(argv[3]);

    printf("========================================================================\n");
    printf("     SCATTER-GATHER (writev) VS LINEAR BUFFER (send) BENCHMARK          \n");
    printf("========================================================================\n");
    printf("Target Server : %s:%d\n", host, port);
    printf("Iterations    : %d requests per test size\n", iterations);

    int fd1 = connect_tnt(host, port);
    if (fd1 < 0) {
        fprintf(stderr, "[-] Failed to connect to Tarantool at %s:%d\n", host, port);
        return 1;
    }

    int fd2 = connect_tnt(host, port);
    if (fd2 < 0) {
        close(fd1);
        fprintf(stderr, "[-] Failed to connect second socket to Tarantool\n");
        return 1;
    }

    /* 1. Small Standard SIP (300 bytes) */
    char sdp_small[] = "v=0\r\no=alice 2890844526 2890844526 IN IP4 172.28.0.50\r\n"
                       "s=VoIP Benchmark Stream\r\nc=IN IP4 172.28.0.50\r\nt=0 0\r\n"
                       "m=audio 32000 RTP/AVP 0 8 96\r\na=rtpmap:0 PCMU/8000\r\n"
                       "a=rtpmap:8 PCMA/8000\r\na=rtpmap:96 opus/48000/2\r\na=sendrecv\r\n";
    run_bench_size(fd1, fd2, iterations, "Small-SDP", sdp_small, strlen(sdp_small));

    /* 2. Large WebRTC SDP (2048 bytes) */
    char sdp_medium[2048];
    memset(sdp_medium, 'B', sizeof(sdp_medium) - 1);
    sdp_medium[sizeof(sdp_medium) - 1] = '\0';
    run_bench_size(fd1, fd2, iterations, "Medium-WebRTC", sdp_medium, strlen(sdp_medium));

    /* 3. Jumbo Multi-Stream SDP (5120 bytes > 4KB stack buffer) */
    char sdp_jumbo[5120];
    memset(sdp_jumbo, 'C', sizeof(sdp_jumbo) - 1);
    sdp_jumbo[sizeof(sdp_jumbo) - 1] = '\0';
    run_bench_size(fd1, fd2, iterations, "Jumbo-SDP-5KB", sdp_jumbo, strlen(sdp_jumbo));

    close(fd1);
    close(fd2);

    printf("\n========================================================================\n");
    printf("[*] Benchmark complete. Scatter-Gather handles all sizes with 0 heap alloc.\n");
    printf("========================================================================\n");
    return 0;
}
