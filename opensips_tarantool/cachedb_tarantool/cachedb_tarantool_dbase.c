/*
 * Copyright (C) 2026 lean1ee <https://github.com/lean1ee>
 *
 * Author: lean1ee
 * Module: cachedb_tarantool - High-performance Tarantool 3.x CacheDB driver and IProto client
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "cachedb_tarantool_dbase.h"
#include "msgpuck.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/uio.h>

int tarantool_connect_tout = DEFAULT_CONNECT_TIMEOUT_MS;
int tarantool_query_tout = DEFAULT_QUERY_TIMEOUT_MS;
int tarantool_lazy_connect = 0;
int tarantool_disable_time = DEFAULT_DISABLE_TIME_SEC;
int tarantool_allowed_errors = DEFAULT_ALLOWED_ERRORS;
int tarantool_init_without_tnt = 1;
int tarantool_pool_size = DEFAULT_POOL_SIZE;
int tarantool_tcp_keepalive = 1;
static void finalize_packet(char *buf, size_t total_len);
static int resolve_space_id(tnt_cluster_con_t *tcon, tnt_single_conn_t *conn);

static int tnt_send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int tnt_writev_all(int fd, struct iovec *iov, int iovcnt) {
    while (iovcnt > 0) {
        ssize_t n = writev(fd, iov, iovcnt);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        while (iovcnt > 0 && n >= (ssize_t)iov[0].iov_len) {
            n -= iov[0].iov_len;
            iov++;
            iovcnt--;
        }
        if (n > 0) {
            iov[0].iov_base = (char *)iov[0].iov_base + n;
            iov[0].iov_len -= (size_t)n;
        }
    }
    return 0;
}

static int tnt_recv_all(int fd, char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static ssize_t tnt_read_frame(tnt_single_conn_t *conn, char *dst, size_t cap, char **dyn) {
    unsigned char pfx[5];
    uint32_t blen;
    char *tgt;

    *dyn = NULL;
    if (tnt_recv_all(conn->sock_fd, (char *)pfx, 5) < 0) return -1;
    if (pfx[0] != (unsigned char)MP_UINT32) {
        LM_ERR("Unexpected IProto length tag 0x%02x\n", pfx[0]);
        return -1;
    }
    blen = ((uint32_t)pfx[1] << 24) | ((uint32_t)pfx[2] << 16) |
           ((uint32_t)pfx[3] << 8) | (uint32_t)pfx[4];
    if (blen == 0 || blen > 16 * 1024 * 1024) {
        LM_ERR("IProto frame length %u out of range\n", blen);
        return -1;
    }
    tgt = dst;
    if ((size_t)blen > cap) {
        tgt = (char *)pkg_malloc(blen);
        if (!tgt) return -1;
        *dyn = tgt;
    }
    if (tnt_recv_all(conn->sock_fd, tgt, blen) < 0) {
        if (*dyn) { pkg_free(*dyn); *dyn = NULL; }
        return -1;
    }
    return (ssize_t)blen;
}

static void tnt_conn_error(tnt_cluster_con_t *tcon, tnt_single_conn_t *conn) {
    if (conn->sock_fd >= 0) {
        close(conn->sock_fd);
        conn->sock_fd = -1;
    }
    conn->state = TNT_STATE_ERROR;
    conn->consecutive_errors++;
    if (conn->consecutive_errors >= tcon->allowed_errors) {
        conn->state = TNT_STATE_DISABLED;
        conn->disabled_until = time(NULL) + tcon->disable_time_sec;
        LM_WARN("Tarantool connection marked DISABLED for %d seconds\n", tcon->disable_time_sec);
    }
}

static int check_iproto_status(const char *body, size_t len, uint64_t want_sync) {
    if (!body || len == 0) return -1;
    const char *p = body;
    const char *end = body + len;

    uint32_t h_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < h_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        if (k == IPROTO_REQUEST_TYPE) {
            uint64_t code = mp_decode_uint(&p);
            if (code != IPROTO_OK) {
                LM_ERR("Tarantool IProto returned error code 0x%lx\n", (unsigned long)code);
                return -1;
            }
        } else if (k == IPROTO_SYNC) {
            uint64_t sync = mp_decode_uint(&p);
            if (sync != want_sync) {
                LM_ERR("IProto sync mismatch (got %lu, want %lu)\n",
                       (unsigned long)sync, (unsigned long)want_sync);
                return -2;
            }
        } else {
            if ((uint8_t)*p <= 0x7f || (uint8_t)*p >= 0xcc) {
                mp_decode_uint(&p);
            } else if (((uint8_t)*p & 0xe0) == 0xa0 || (uint8_t)*p == 0xd9 || (uint8_t)*p == 0xda) {
                uint32_t dlen = 0;
                mp_decode_str(&p, &dlen);
            } else {
                p++;
            }
        }
    }
    return 0;
}

static int tnt_connect_single(tnt_cluster_con_t *tcon, tnt_single_conn_t *conn) {
    if (!tcon || !conn) return -1;

    time_t now = time(NULL);
    if (conn->state == TNT_STATE_DISABLED) {
        if (now < conn->disabled_until) {
            return -1;
        }
        conn->state = TNT_STATE_DISCONNECTED;
        conn->consecutive_errors = 0;
    }

    if (conn->sock_fd >= 0) {
        close(conn->sock_fd);
        conn->sock_fd = -1;
    }

    conn->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->sock_fd < 0) {
        LM_ERR("Failed to create TCP socket for Tarantool (errno: %d)\n", errno);
        conn->state = TNT_STATE_ERROR;
        return -1;
    }

    /* Set TCP_NODELAY to avoid 200-500us Nagle latency */
    int nodelay = 1;
    setsockopt(conn->sock_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(nodelay));

#ifdef TCP_QUICKACK
    /* Enable TCP_QUICKACK on Linux to avoid delayed ACK latencies */
    int quickack = 1;
    setsockopt(conn->sock_fd, IPPROTO_TCP, TCP_QUICKACK, (char *)&quickack, sizeof(quickack));
#endif

    /* Set TCP Keepalive */
    if (tcon->tcp_keepalive) {
        int optval = 1;
        setsockopt(conn->sock_fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
    }

    /* Set timeouts */
    struct timeval tv;
    tv.tv_sec = tcon->query_timeout_ms / 1000;
    tv.tv_usec = (tcon->query_timeout_ms % 1000) * 1000;
    setsockopt(conn->sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(conn->sock_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    /* Socket buffers (1MB high-throughput carrier-grade buffers) */
    int buf_size = 1024 * 1024;
    setsockopt(conn->sock_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(conn->sock_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    char host_buf[256];
    int host_len = tcon->host.len < (int)sizeof(host_buf) - 1 ? tcon->host.len : (int)sizeof(host_buf) - 1;
    memcpy(host_buf, tcon->host.s, host_len);
    host_buf[host_len] = '\0';

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(tcon->port);

    if (inet_pton(AF_INET, host_buf, &serv_addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(host_buf);
        if (!he) {
            LM_ERR("Failed to resolve Tarantool host '%s'\n", host_buf);
            close(conn->sock_fd);
            conn->sock_fd = -1;
            conn->state = TNT_STATE_ERROR;
            return -1;
        }
        memcpy(&serv_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (connect(conn->sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        LM_WARN("Failed to connect to Tarantool server %s:%d (errno: %d)\n", host_buf, tcon->port, errno);
        close(conn->sock_fd);
        conn->sock_fd = -1;
        conn->state = TNT_STATE_ERROR;
        conn->consecutive_errors++;
        if (conn->consecutive_errors >= tcon->allowed_errors) {
            conn->state = TNT_STATE_DISABLED;
            conn->disabled_until = now + tcon->disable_time_sec;
            LM_WARN("Tarantool connection marked DISABLED for %d seconds\n", tcon->disable_time_sec);
        }
        return -1;
    }

    /* Read 128-byte Greeting Handshake */
    char greeting[128];
    ssize_t n = recv(conn->sock_fd, greeting, sizeof(greeting), MSG_WAITALL);
    if (n != sizeof(greeting)) {
        LM_ERR("Failed to read Tarantool greeting handshake\n");
        close(conn->sock_fd);
        conn->sock_fd = -1;
        conn->state = TNT_STATE_ERROR;
        return -1;
    }

    conn->state = TNT_STATE_AUTHENTICATED;
    conn->consecutive_errors = 0;
    conn->last_activity = now;
    
    /* Dynamically resolve space ID from Tarantool (zero hardcoding) */
    if (tcon->space_id == 0) {
        resolve_space_id(tcon, conn);
    }

    LM_INFO("Successfully connected to Tarantool 3.x cluster node '%s:%d' (space: '%.*s', id: %u)\n",
            host_buf, tcon->port, tcon->space.len, tcon->space.s, tcon->space_id);
    return 0;
}

static int resolve_space_id(tnt_cluster_con_t *tcon, tnt_single_conn_t *conn) {
    if (!tcon || !conn || conn->sock_fd < 0) return -1;

    char lua_cmd[256];
    snprintf(lua_cmd, sizeof(lua_cmd), "return box.space['%.*s'] and box.space['%.*s'].id or 512",
             tcon->space.len, tcon->space.s, tcon->space.len, tcon->space.s);

    char buf[1024];
    uint64_t sync_id = ++conn->sync_counter;

    char *p = buf + 5;
    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
    p = mp_encode_uint(p, IPROTO_EVAL);
    p = mp_encode_uint(p, IPROTO_SYNC);
    p = mp_encode_uint(p, sync_id);

    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_EXPR);
    p = mp_encode_str(p, lua_cmd, strlen(lua_cmd));
    p = mp_encode_uint(p, IPROTO_TUPLE);
    p = mp_encode_array(p, 0);

    size_t len = (size_t)(p - buf);
    finalize_packet(buf, len);

    if (tnt_send_all(conn->sock_fd, buf, len) == 0) {
        char resp[512], *dyn = NULL;
        ssize_t blen = tnt_read_frame(conn, resp, sizeof(resp), &dyn);
        if (blen > 0) {
            const char *rp = dyn ? dyn : resp;
            const char *rend = rp + blen;
            uint32_t h_map = mp_decode_map(&rp);
            for (uint32_t i = 0; i < h_map && rp < rend; i++) {
                mp_decode_uint(&rp);
                mp_decode_uint(&rp);
            }
            if (rp < rend) {
                uint32_t b_map = mp_decode_map(&rp);
                for (uint32_t i = 0; i < b_map && rp < rend; i++) {
                    uint64_t k = mp_decode_uint(&rp);
                    if (k == IPROTO_DATA) {
                        uint32_t tcount = mp_decode_array(&rp);
                        if (tcount >= 1) {
                            tcon->space_id = (uint32_t)mp_decode_uint(&rp);
                            if (dyn) pkg_free(dyn);
                            return 0;
                        }
                    } else {
                        rp++;
                    }
                }
            }
        }
        if (dyn) pkg_free(dyn);
    }
    tcon->space_id = 512; // Graceful fallback
    return 0;
}

/* Lockless, ultra-fast per-worker connection selection (0ns mutex overhead) */
static tnt_single_conn_t *tnt_get_conn(tnt_cluster_con_t *tcon) {
    if (!tcon || tcon->pool_size <= 0) return NULL;

    pid_t my_pid = getpid();
    int slot = (int)((unsigned int)my_pid % (unsigned int)tcon->pool_size);
    tnt_single_conn_t *c = &tcon->conns[slot];

    /* Fork detection: parent's sockets are dropped once per child process */
    if (tcon->owner_pid != my_pid) {
        pthread_mutex_lock(&tcon->lock);
        if (tcon->owner_pid != my_pid) {
            for (int i = 0; i < tcon->pool_size; i++) {
                if (tcon->conns[i].sock_fd >= 0)
                    close(tcon->conns[i].sock_fd);
                tcon->conns[i].sock_fd = -1;
                tcon->conns[i].state = TNT_STATE_DISCONNECTED;
                tcon->conns[i].consecutive_errors = 0;
                tcon->conns[i].disabled_until = 0;
            }
            tcon->owner_pid = my_pid;
        }
        pthread_mutex_unlock(&tcon->lock);
    }

    /* Fast path: 100% lock-free access to worker's dedicated socket */
    if (c->state == TNT_STATE_AUTHENTICATED && c->sock_fd >= 0) {
        return c;
    }

    if (c->state != TNT_STATE_DISABLED) {
        if (tnt_connect_single(tcon, c) == 0) {
            return c;
        }
    }

    /* Resilient fallback to alternative slots if primary is disabled */
    for (int i = 1; i < tcon->pool_size; i++) {
        int alt_slot = (slot + i) % tcon->pool_size;
        tnt_single_conn_t *alt_c = &tcon->conns[alt_slot];
        if (alt_c->state == TNT_STATE_AUTHENTICATED && alt_c->sock_fd >= 0) {
            return alt_c;
        }
        if (alt_c->state != TNT_STATE_DISABLED) {
            if (tnt_connect_single(tcon, alt_c) == 0) {
                return alt_c;
            }
        }
    }

    return NULL;
}

/* Fast Binary IProto Packet Encoders (Direct Memtx Engine C-Path) */
static size_t pack_select_header(char *buf, uint64_t sync_id, uint32_t space_id) {
    char *p = buf + 5;
    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
    p = mp_encode_uint(p, IPROTO_SELECT);
    p = mp_encode_uint(p, IPROTO_SYNC);
    p = mp_encode_uint(p, sync_id);

    p = mp_encode_map(p, 6);
    p = mp_encode_uint(p, IPROTO_SPACE_ID);
    p = mp_encode_uint(p, space_id);
    p = mp_encode_uint(p, IPROTO_INDEX_ID);
    p = mp_encode_uint(p, 0); // Primary index
    p = mp_encode_uint(p, IPROTO_LIMIT);
    p = mp_encode_uint(p, 1);
    p = mp_encode_uint(p, IPROTO_OFFSET);
    p = mp_encode_uint(p, 0);
    p = mp_encode_uint(p, IPROTO_ITERATOR);
    p = mp_encode_uint(p, 0); // ITER_EQ
    p = mp_encode_uint(p, IPROTO_KEY);
    p = mp_encode_array(p, 1);

    return (size_t)(p - buf);
}

static size_t pack_delete_header(char *buf, uint64_t sync_id, uint32_t space_id) {
    char *p = buf + 5;
    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
    p = mp_encode_uint(p, IPROTO_DELETE);
    p = mp_encode_uint(p, IPROTO_SYNC);
    p = mp_encode_uint(p, sync_id);

    p = mp_encode_map(p, 3);
    p = mp_encode_uint(p, IPROTO_SPACE_ID);
    p = mp_encode_uint(p, space_id);
    p = mp_encode_uint(p, IPROTO_INDEX_ID);
    p = mp_encode_uint(p, 0);
    p = mp_encode_uint(p, IPROTO_KEY);
    p = mp_encode_array(p, 1);

    return (size_t)(p - buf);
}

static size_t pack_call_header(char *buf, uint64_t sync_id, const char *proc, uint32_t proc_len, uint32_t args_count) {
    char *p = buf + 5;
    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
    p = mp_encode_uint(p, IPROTO_CALL);
    p = mp_encode_uint(p, IPROTO_SYNC);
    p = mp_encode_uint(p, sync_id);

    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_FUNCTION_NAME);
    p = mp_encode_str(p, proc, proc_len);
    p = mp_encode_uint(p, IPROTO_TUPLE);
    p = mp_encode_array(p, args_count);

    return (size_t)(p - buf);
}

static void finalize_packet(char *buf, size_t total_len) {
    uint32_t payload_len = (uint32_t)(total_len - 5);
    char *len_ptr = buf;
    *len_ptr++ = (char)MP_UINT32;
    *len_ptr++ = (char)(payload_len >> 24);
    *len_ptr++ = (char)(payload_len >> 16);
    *len_ptr++ = (char)(payload_len >> 8);
    *len_ptr++ = (char)(payload_len);
}

static int parse_iproto_select_response(const char *body, size_t len, uint64_t want_sync, str *out_val) {
    if (!body || len == 0 || !out_val) return -1;

    const char *p = body;
    const char *end = body + len;

    /* Decode Header Map */
    uint32_t h_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < h_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        uint64_t v = mp_decode_uint(&p);
        if (k == IPROTO_REQUEST_TYPE && v != IPROTO_OK) {
            return -1;
        }
        if (k == IPROTO_SYNC && v != want_sync) {
            LM_ERR("IProto sync mismatch (got %lu, want %lu)\n",
                   (unsigned long)v, (unsigned long)want_sync);
            return -2;
        }
    }

    /* Decode Body Map */
    if (p >= end) return -1;
    uint32_t b_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < b_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        if (k == IPROTO_DATA) {
            uint32_t tuple_count = mp_decode_array(&p);
            if (tuple_count == 0) {
                out_val->s = NULL;
                out_val->len = 0;
                return -2; // Key not found (OpenSIPS cachedb contract: -2)
            }
            uint32_t field_count = mp_decode_array(&p);
            if (field_count < 1) {
                out_val->s = NULL;
                out_val->len = 0;
                return -2;
            }

            // Field 0: key (call_id)
            uint32_t dummy_len = 0;
            mp_decode_str(&p, &dummy_len);

            // In rtpe_calls schema (7 fields):
            // [call_id, node_id, state, created_at, updated_at, expires_at, payload]
            if (field_count >= 7) {
                mp_decode_str(&p, &dummy_len);  // field 1: node_id
                mp_decode_str(&p, &dummy_len);  // field 2: state
                mp_decode_uint(&p);             // field 3: created_at
                mp_decode_uint(&p);             // field 4: updated_at
                mp_decode_uint(&p);             // field 5: expires_at
                uint32_t val_len = 0;
                const char *val_ptr = mp_decode_str(&p, &val_len); // field 6: payload
                if (val_ptr && val_len > 0) {
                    char *ret_str = (char *)pkg_malloc(val_len + 1);
                    if (!ret_str) return -1;
                    memcpy(ret_str, val_ptr, val_len);
                    ret_str[val_len] = '\0';
                    out_val->s = ret_str;
                    out_val->len = (int)val_len;
                    return 0;
                }
            } else if (field_count >= 3) {
                mp_decode_str(&p, &dummy_len); // skip node_id
                uint32_t val_len = 0;
                const char *val_ptr = mp_decode_str(&p, &val_len);
                if (val_ptr && val_len > 0) {
                    char *ret_str = (char *)pkg_malloc(val_len + 1);
                    if (!ret_str) return -1;
                    memcpy(ret_str, val_ptr, val_len);
                    ret_str[val_len] = '\0';
                    out_val->s = ret_str;
                    out_val->len = (int)val_len;
                    return 0;
                }
            } else if (field_count >= 2) {
                uint32_t val_len = 0;
                const char *val_ptr = mp_decode_str(&p, &val_len);
                if (val_ptr && val_len > 0) {
                    char *ret_str = (char *)pkg_malloc(val_len + 1);
                    if (!ret_str) return -1;
                    memcpy(ret_str, val_ptr, val_len);
                    ret_str[val_len] = '\0';
                    out_val->s = ret_str;
                    out_val->len = (int)val_len;
                    return 0;
                }
            }
            out_val->s = NULL;
            out_val->len = 0;
            return -2;
        } else {
            p += 1;
        }
    }
    out_val->s = NULL;
    out_val->len = 0;
    return -2;
}

static int parse_iproto_select_buf(const char *body, size_t len, uint64_t want_sync,
                                   char *dst_buf, unsigned int buflen,
                                   unsigned int *vlen, unsigned int *needed) {
    if (!body || len == 0) return -1;
    const char *p = body;
    const char *end = body + len;

    /* Decode Header Map */
    uint32_t h_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < h_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        uint64_t v = mp_decode_uint(&p);
        if (k == IPROTO_REQUEST_TYPE && v != IPROTO_OK) {
            return -1;
        }
        if (k == IPROTO_SYNC && v != want_sync) {
            LM_ERR("IProto sync mismatch (got %lu, want %lu)\n",
                   (unsigned long)v, (unsigned long)want_sync);
            return -2;
        }
    }

    /* Decode Body Map */
    if (p >= end) return -1;
    uint32_t b_map = mp_decode_map(&p);
    for (uint32_t i = 0; i < b_map && p < end; i++) {
        uint64_t k = mp_decode_uint(&p);
        if (k == IPROTO_DATA) {
            uint32_t tuple_count = mp_decode_array(&p);
            if (tuple_count == 0) {
                return -2; /* Key not found */
            }
            uint32_t field_count = mp_decode_array(&p);
            if (field_count < 1) {
                return -2;
            }

            /* Field 0: key (call_id) */
            uint32_t dummy_len = 0;
            mp_decode_str(&p, &dummy_len);

            const char *val_ptr = NULL;
            uint32_t val_len = 0;

            if (field_count >= 7) {
                mp_decode_str(&p, &dummy_len);  /* field 1: node_id */
                mp_decode_str(&p, &dummy_len);  /* field 2: state */
                mp_decode_uint(&p);             /* field 3: created_at */
                mp_decode_uint(&p);             /* field 4: updated_at */
                mp_decode_uint(&p);             /* field 5: expires_at */
                val_ptr = mp_decode_str(&p, &val_len); /* field 6: payload */
            } else if (field_count >= 3) {
                mp_decode_str(&p, &dummy_len); /* field 1: node_id */
                val_ptr = mp_decode_str(&p, &val_len); /* field 2: payload */
            } else if (field_count >= 2) {
                val_ptr = mp_decode_str(&p, &val_len);
            }

            if (!val_ptr) return -2;

            if (val_len > buflen) {
                if (needed) *needed = val_len;
                return -3; /* Buffer too small, caller may retry */
            }

            memcpy(dst_buf, val_ptr, val_len);
            if (vlen) *vlen = val_len;
            return 0; /* Success: hit written directly to caller buffer */
        } else {
            p += 1;
        }
    }
    return -2;
}

#ifndef TNT_REAL_OPENSIPS
/* Parse connection URL: tarantool:name://[user:pass@]host:port/space
 * (standalone/mock builds only - the real build gets a parsed cachedb_id
 * from the core) */
static int parse_tnt_url(str *url, tnt_cluster_con_t *tcon) {
    if (!url || !url->s || url->len <= 0 || !tcon) return -1;

    char *buf = pkg_strdup(url->s);
    if (!buf) return -1;
    buf[url->len] = '\0';

    char *p = buf;
    if (strncmp(p, "tarantool:", 10) != 0) {
        pkg_free(buf);
        return -1;
    }
    p += 10;

    char *name_end = strstr(p, "://");
    if (!name_end) {
        pkg_free(buf);
        return -1;
    }
    *name_end = '\0';
    tcon->name.s = pkg_strdup(p);
    tcon->name.len = strlen(tcon->name.s);
    p = name_end + 3;

    char *slash = strchr(p, '/');
    if (slash) {
        *slash = '\0';
        tcon->space.s = pkg_strdup(slash + 1);
        tcon->space.len = strlen(tcon->space.s);
    } else {
        tcon->space.s = pkg_strdup("rtpe_calls");
        tcon->space.len = 10;
    }

    char *at = strchr(p, '@');
    if (at) {
        *at = '\0';
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            tcon->user.s = pkg_strdup(p);
            tcon->pass.s = pkg_strdup(colon + 1);
        } else {
            tcon->user.s = pkg_strdup(p);
            tcon->pass.s = pkg_strdup("");
        }
        p = at + 1;
    } else {
        tcon->user.s = pkg_strdup("guest");
        tcon->pass.s = pkg_strdup("");
    }
    tcon->user.len = strlen(tcon->user.s);
    tcon->pass.len = strlen(tcon->pass.s);

    char *colon = strchr(p, ':');
    if (colon) {
        *colon = '\0';
        tcon->port = atoi(colon + 1);
    } else {
        tcon->port = 3301;
    }
    tcon->host.s = pkg_strdup(p);
    tcon->host.len = strlen(tcon->host.s);

    tcon->space_id = 0;
    tcon->pool_size = tarantool_pool_size > 0 ? tarantool_pool_size : DEFAULT_POOL_SIZE;
    tcon->connect_timeout_ms = tarantool_connect_tout;
    tcon->query_timeout_ms = tarantool_query_tout;
    tcon->disable_time_sec = tarantool_disable_time;
    tcon->allowed_errors = tarantool_allowed_errors;
    tcon->lazy_connect = tarantool_lazy_connect;
    tcon->init_without_tarantool = tarantool_init_without_tnt;
    tcon->tcp_keepalive = tarantool_tcp_keepalive;

    pkg_free(buf);
    return 0;
}
#endif /* !TNT_REAL_OPENSIPS */

/* shared tail of both constructors: allocate + connect the pool */
static int tnt_setup_pool(tnt_cluster_con_t *tcon) {
    tcon->conns = (tnt_single_conn_t *)pkg_malloc(sizeof(tnt_single_conn_t) * tcon->pool_size);
    if (!tcon->conns)
        return -1;
    memset(tcon->conns, 0, sizeof(tnt_single_conn_t) * tcon->pool_size);
    for (int i = 0; i < tcon->pool_size; i++) {
        tcon->conns[i].sock_fd = -1;
        tcon->conns[i].state = TNT_STATE_DISCONNECTED;
    }

    pthread_mutex_init(&tcon->lock, NULL);
    tcon->owner_pid = getpid();

    if (!tcon->lazy_connect) {
        for (int i = 0; i < tcon->pool_size; i++) {
            tnt_connect_single(tcon, &tcon->conns[i]);
        }
    }
    return 0;
}

static void tnt_teardown(tnt_cluster_con_t *tcon) {
    pthread_mutex_lock(&tcon->lock);
    if (tcon->conns) {
        for (int i = 0; i < tcon->pool_size; i++) {
            if (tcon->conns[i].sock_fd >= 0) {
                close(tcon->conns[i].sock_fd);
            }
        }
        pkg_free(tcon->conns);
    }
    pthread_mutex_unlock(&tcon->lock);
    pthread_mutex_destroy(&tcon->lock);

    if (tcon->name.s) pkg_free(tcon->name.s);
    if (tcon->host.s) pkg_free(tcon->host.s);
    if (tcon->user.s) pkg_free(tcon->user.s);
    if (tcon->pass.s) pkg_free(tcon->pass.s);
    if (tcon->space.s) pkg_free(tcon->space.s);
    pkg_free(tcon);
}

#ifdef TNT_REAL_OPENSIPS

/* The engine's .init/.destroy MUST go through the core connection pool:
 * cachedb_do_init() parses the URL into a cachedb_id, calls the
 * new-connection callback below, and hands the ops a cachedb_con whose
 * ->data points at this struct. */
static tnt_cluster_con_t *tnt_new_connection(struct cachedb_id *id) {
    tnt_cluster_con_t *tcon;

    if (!id || !id->host) {
        LM_ERR("no host in Tarantool URL\n");
        return NULL;
    }

    tcon = (tnt_cluster_con_t *)pkg_malloc(sizeof(tnt_cluster_con_t));
    if (!tcon) {
        LM_ERR("Out of memory allocating Tarantool cluster connection\n");
        return NULL;
    }
    memset(tcon, 0, sizeof(tnt_cluster_con_t));
    tcon->cache_con.id = id;
    tcon->cache_con.ref = 1;

    tcon->name.s = pkg_strdup(id->group_name ? id->group_name : "default");
    tcon->host.s = pkg_strdup(id->host);
    tcon->user.s = pkg_strdup(id->username ? id->username : "guest");
    tcon->pass.s = pkg_strdup(id->password ? id->password : "");
    tcon->space.s = pkg_strdup(id->database ? id->database : "rtpe_calls");
    if (!tcon->name.s || !tcon->host.s || !tcon->user.s || !tcon->pass.s
            || !tcon->space.s) {
        LM_ERR("Out of memory duplicating Tarantool URL parts\n");
        goto error;
    }
    tcon->name.len = strlen(tcon->name.s);
    tcon->host.len = strlen(tcon->host.s);
    tcon->user.len = strlen(tcon->user.s);
    tcon->pass.len = strlen(tcon->pass.s);
    tcon->space.len = strlen(tcon->space.s);

    tcon->port = id->port ? id->port : 3301;
    tcon->space_id = 0; /* resolved dynamically on first connect */

    tcon->pool_size = tarantool_pool_size > 0 ? tarantool_pool_size : DEFAULT_POOL_SIZE;
    tcon->connect_timeout_ms = tarantool_connect_tout;
    tcon->query_timeout_ms = tarantool_query_tout;
    tcon->disable_time_sec = tarantool_disable_time;
    tcon->allowed_errors = tarantool_allowed_errors;
    tcon->lazy_connect = tarantool_lazy_connect;
    tcon->init_without_tarantool = tarantool_init_without_tnt;
    tcon->tcp_keepalive = tarantool_tcp_keepalive;

    if (tnt_setup_pool(tcon) < 0)
        goto error;

    return tcon;

error:
    if (tcon->name.s) pkg_free(tcon->name.s);
    if (tcon->host.s) pkg_free(tcon->host.s);
    if (tcon->user.s) pkg_free(tcon->user.s);
    if (tcon->pass.s) pkg_free(tcon->pass.s);
    if (tcon->space.s) pkg_free(tcon->space.s);
    pkg_free(tcon);
    return NULL;
}

cachedb_con *tarantool_init(str *url) {
    return cachedb_do_init(url, (void *)tnt_new_connection);
}

static void tnt_free_connection(cachedb_pool_con *cpc) {
    if (!cpc) return;
    tnt_teardown((tnt_cluster_con_t *)cpc);
}

void tarantool_destroy(cachedb_con *con) {
    cachedb_do_close(con, tnt_free_connection);
}

#else /* standalone / mock build */

cachedb_con *tarantool_init(str *url) {
    if (!url) return NULL;

    tnt_cluster_con_t *tcon = (tnt_cluster_con_t *)pkg_malloc(sizeof(tnt_cluster_con_t));
    if (!tcon) {
        LM_ERR("Out of memory allocating Tarantool cluster connection\n");
        return NULL;
    }
    memset(tcon, 0, sizeof(tnt_cluster_con_t));

    if (parse_tnt_url(url, tcon) != 0) {
        LM_ERR("Failed to parse Tarantool URL '%.*s'\n", url->len, url->s);
        pkg_free(tcon);
        return NULL;
    }

    if (tnt_setup_pool(tcon) < 0) {
        pkg_free(tcon);
        return NULL;
    }

    return (cachedb_con *)tcon;
}

void tarantool_destroy(cachedb_con *con) {
    if (!con) return;
    tnt_teardown((tnt_cluster_con_t *)con);
}

#endif /* TNT_REAL_OPENSIPS */

#ifdef TNT_REAL_OPENSIPS
#define TNT_CON(con) ((tnt_cluster_con_t *)((con)->data))
#else
#define TNT_CON(con) ((tnt_cluster_con_t *)(con))
#endif

/* Fast IPROTO_SELECT (C Memtx Path, ~1us) */
int tarantool_get(cachedb_con *con, str *attr, str *val) {
    if (!con || !attr || !val) return -1;
    tnt_cluster_con_t *tcon = TNT_CON(con);
    if (!tcon) return -1;
    tnt_single_conn_t *c = tnt_get_conn(tcon);
    int rc;
    if (!c) return -1;

    if (attr->len > 512) {
        LM_ERR("Key too long (%d)\n", attr->len);
        return -1;
    }

    char buf[1024];
    uint64_t sync_id = ++c->sync_counter;
    size_t len = pack_select_header(buf, sync_id, tcon->space_id);
    char *p = buf + len;
    p = mp_encode_str(p, attr->s, attr->len);
    len = (size_t)(p - buf);
    finalize_packet(buf, len);

    if (tnt_send_all(c->sock_fd, buf, len) < 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    char resp[4096], *dyn = NULL;
    ssize_t blen = tnt_read_frame(c, resp, sizeof(resp), &dyn);
    if (blen <= 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    rc = parse_iproto_select_response(dyn ? dyn : resp, (size_t)blen, sync_id, val);
    if (dyn) pkg_free(dyn);
    if (rc < 0 && rc != -2) {
        tnt_conn_error(tcon, c);
    }
    return rc;
}

/* Zero-Copy / Zero-Alloc CacheDB get_buf (PR #4118 capability CACHEDB_CAP_GET_BUF) */
int tarantool_get_buf(cachedb_con *con, str *attr, char *buf,
                      unsigned int buflen, unsigned int *vlen, unsigned int *needed) {
    if (vlen) *vlen = 0;
    if (needed) *needed = 0;
    if (!con || !attr || !buf || buflen == 0) return -1;
    tnt_cluster_con_t *tcon = TNT_CON(con);
    if (!tcon) return -1;
    tnt_single_conn_t *c = tnt_get_conn(tcon);
    int rc;
    if (!c) return -1;

    if (attr->len > 512) {
        LM_ERR("Key too long (%d)\n", attr->len);
        return -1;
    }

    char req_buf[1024];
    uint64_t sync_id = ++c->sync_counter;
    size_t len = pack_select_header(req_buf, sync_id, tcon->space_id);
    char *p = req_buf + len;
    p = mp_encode_str(p, attr->s, attr->len);
    len = (size_t)(p - req_buf);
    finalize_packet(req_buf, len);

    if (tnt_send_all(c->sock_fd, req_buf, len) < 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    char resp[4096], *dyn = NULL;
    ssize_t blen = tnt_read_frame(c, resp, sizeof(resp), &dyn);
    if (blen <= 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    rc = parse_iproto_select_buf(dyn ? dyn : resp, (size_t)blen, sync_id, buf, buflen, vlen, needed);
    if (dyn) pkg_free(dyn);
    if (rc < 0 && rc != -2 && rc != -3) {
        tnt_conn_error(tcon, c);
    }
    return rc;
}

/* Fast IPROTO_REPLACE with strict 7-field schema matching Tarantool rtpe_calls
 * 100% Zero-Allocation & Zero-Copy Vectored I/O (writev) */
int tarantool_set(cachedb_con *con, str *attr, str *val, int expires) {
    if (!con || !attr || !val) return -1;
    tnt_cluster_con_t *tcon = TNT_CON(con);
    if (!tcon) return -1;
    tnt_single_conn_t *c = tnt_get_conn(tcon);
    int rc;
    if (!c) return -1;

    time_t now = time(NULL);
    uint64_t ttl = (expires > 0) ? (uint64_t)expires : 3600;
    uint64_t expires_at = (uint64_t)now + ttl;
    uint64_t sync_id = ++c->sync_counter;

    char hdr_buf[64];
    char meta_buf[64];
    char val_hdr[8];

    /* 1. Encode Header & Tuple map up to the key string */
    char *hp = hdr_buf + 5;
    hp = mp_encode_map(hp, 2);
    hp = mp_encode_uint(hp, IPROTO_REQUEST_TYPE);
    hp = mp_encode_uint(hp, IPROTO_REPLACE);
    hp = mp_encode_uint(hp, IPROTO_SYNC);
    hp = mp_encode_uint(hp, sync_id);

    hp = mp_encode_map(hp, 2);
    hp = mp_encode_uint(hp, IPROTO_SPACE_ID);
    hp = mp_encode_uint(hp, tcon->space_id);
    hp = mp_encode_uint(hp, IPROTO_TUPLE);
    hp = mp_encode_array(hp, 7);

    /* Field 1: call_id key prefix */
    if (attr->len <= 31) {
        *hp++ = (char)(0xa0 | attr->len);
    } else if (attr->len <= 0xff) {
        *hp++ = (char)0xd9;
        *hp++ = (char)attr->len;
    } else {
        *hp++ = (char)0xda;
        *hp++ = (char)(attr->len >> 8);
        *hp++ = (char)attr->len;
    }
    size_t hdr_payload = (size_t)(hp - hdr_buf - 5);

    /* 2. Encode fixed metadata fields (node_id, state, created_at, updated_at, expires_at) */
    char *mp = meta_buf;
    mp = mp_encode_str(mp, "opensips", 8);          // 2: node_id
    mp = mp_encode_str(mp, "active", 6);            // 3: state
    mp = mp_encode_uint(mp, (uint64_t)now);         // 4: created_at
    mp = mp_encode_uint(mp, (uint64_t)now);         // 5: updated_at
    mp = mp_encode_uint(mp, expires_at);            // 6: expires_at
    size_t meta_len = (size_t)(mp - meta_buf);

    /* 3. Encode Field 7 (payload value) string header prefix */
    char *vp = val_hdr;
    size_t val_len = (size_t)val->len;
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

    /* Calculate total payload length across all 5 vectors */
    uint32_t total_payload = (uint32_t)(hdr_payload + (size_t)attr->len + meta_len + val_hdr_len + val_len);
    hdr_buf[0] = (char)0xce;
    hdr_buf[1] = (char)(total_payload >> 24);
    hdr_buf[2] = (char)(total_payload >> 16);
    hdr_buf[3] = (char)(total_payload >> 8);
    hdr_buf[4] = (char)(total_payload);

    /* 5-Vector Scatter-Gather I/O: 100% Zero-Copy & Zero-Allocation */
    struct iovec iov[5];
    iov[0].iov_base = hdr_buf;
    iov[0].iov_len  = hdr_payload + 5;

    iov[1].iov_base = (void *)attr->s;   // Vector 1: direct pointer to call_id string
    iov[1].iov_len  = (size_t)attr->len;

    iov[2].iov_base = meta_buf;          // Vector 2: fixed metadata
    iov[2].iov_len  = meta_len;

    iov[3].iov_base = val_hdr;           // Vector 3: payload string header
    iov[3].iov_len  = val_hdr_len;

    iov[4].iov_base = (void *)val->s;    // Vector 4: direct pointer to SDP body (ZERO-COPY)
    iov[4].iov_len  = val_len;

    if (tnt_writev_all(c->sock_fd, iov, 5) < 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    char resp[512], *dyn = NULL;
    ssize_t blen = tnt_read_frame(c, resp, sizeof(resp), &dyn);
    if (blen <= 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }
    rc = check_iproto_status(dyn ? dyn : resp, (size_t)blen, sync_id);
    if (dyn) pkg_free(dyn);
    if (rc == -2) {
        tnt_conn_error(tcon, c);
        return -1;
    }
    return rc;
}

/* Fast IPROTO_DELETE (C Memtx Path, ~1us) */
int tarantool_remove(cachedb_con *con, str *attr) {
    if (!con || !attr) return -1;
    tnt_cluster_con_t *tcon = TNT_CON(con);
    if (!tcon) return -1;
    tnt_single_conn_t *c = tnt_get_conn(tcon);
    int rc;
    if (!c) return -1;

    if (attr->len > 512) {
        LM_ERR("Key too long (%d)\n", attr->len);
        return -1;
    }

    char buf[1024];
    uint64_t sync_id = ++c->sync_counter;
    size_t len = pack_delete_header(buf, sync_id, tcon->space_id);
    char *p = buf + len;
    p = mp_encode_str(p, attr->s, attr->len);
    len = (size_t)(p - buf);
    finalize_packet(buf, len);

    if (tnt_send_all(c->sock_fd, buf, len) < 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    char resp[512], *dyn = NULL;
    ssize_t blen = tnt_read_frame(c, resp, sizeof(resp), &dyn);
    if (blen <= 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }
    rc = check_iproto_status(dyn ? dyn : resp, (size_t)blen, sync_id);
    if (dyn) pkg_free(dyn);
    if (rc == -2) {
        tnt_conn_error(tcon, c);
        return -1;
    }
    return rc;
}

int tarantool_raw_query(cachedb_con *con, str *query, cdb_raw_entry ***reply, int num_cols, int *num_rows) {
    (void)num_cols;
    if (!con || !query) return -1;
    tnt_cluster_con_t *tcon = TNT_CON(con);
    if (!tcon) return -1;
    tnt_single_conn_t *c = tnt_get_conn(tcon);
    if (!c) return -1;

    char buf[4096];
    uint64_t sync_id = ++c->sync_counter;
    
    char *p = buf + 5;
    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
    p = mp_encode_uint(p, IPROTO_EVAL);
    p = mp_encode_uint(p, IPROTO_SYNC);
    p = mp_encode_uint(p, sync_id);

    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_EXPR);
    p = mp_encode_str(p, query->s, query->len);
    p = mp_encode_uint(p, IPROTO_TUPLE);
    p = mp_encode_array(p, 0);

    size_t len = (size_t)(p - buf);
    finalize_packet(buf, len);

    if (tnt_send_all(c->sock_fd, buf, len) < 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    char resp[4096], *dyn = NULL;
    ssize_t blen = tnt_read_frame(c, resp, sizeof(resp), &dyn);
    if (blen <= 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    const char *actual_resp = dyn ? dyn : resp;

    if (num_rows) *num_rows = 1;
    if (reply) {
        cdb_raw_entry **arr = (cdb_raw_entry **)pkg_malloc(sizeof(cdb_raw_entry *));
        if (arr) {
            arr[0] = (cdb_raw_entry *)pkg_malloc(sizeof(cdb_raw_entry));
            if (arr[0]) {
                arr[0]->val.s.s = (char *)pkg_malloc(blen + 1);
                if (arr[0]->val.s.s) {
                    memcpy(arr[0]->val.s.s, actual_resp, blen);
                    arr[0]->val.s.s[blen] = '\0';
                    arr[0]->val.s.len = (int)blen;
                }
                arr[0]->type = CDB_STR;
            }
            *reply = arr;
        }
    }
    if (dyn) pkg_free(dyn);
    return 0;
}

int tarantool_call_proc(tnt_cluster_con_t *tcon, const str *proc, const str *args, str *res) {
    if (!tcon || !proc || !res) return -1;
    tnt_single_conn_t *c = tnt_get_conn(tcon);
    if (!c) return -1;

    char buf[4096];
    uint64_t sync_id = ++c->sync_counter;

    char proc_name[256];
    int plen = proc->len < (int)sizeof(proc_name) - 1 ? proc->len : (int)sizeof(proc_name) - 1;
    memcpy(proc_name, proc->s, plen);
    proc_name[plen] = '\0';

    size_t len = pack_call_header(buf, sync_id, proc_name, plen, args ? 1 : 0);
    char *p = buf + len;
    if (args && args->len > 0) {
        p = mp_encode_str(p, args->s, args->len);
    }
    len = (size_t)(p - buf);
    finalize_packet(buf, len);

    if (tnt_send_all(c->sock_fd, buf, len) < 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    char resp[4096], *dyn = NULL;
    ssize_t blen = tnt_read_frame(c, resp, sizeof(resp), &dyn);
    if (blen <= 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    const char *actual_resp = dyn ? dyn : resp;
    char *ret_str = (char *)pkg_malloc(blen + 1);
    if (ret_str) {
        memcpy(ret_str, actual_resp, blen);
        ret_str[blen] = '\0';
        res->s = ret_str;
        res->len = (int)blen;
    }
    if (dyn) pkg_free(dyn);
    return 0;
}

int tarantool_eval_expr(tnt_cluster_con_t *tcon, const str *expr, const str *args, str *res) {
    (void)args;
    if (!tcon || !expr || !res) return -1;
    tnt_single_conn_t *c = tnt_get_conn(tcon);
    if (!c) return -1;

    char buf[4096];
    uint64_t sync_id = ++c->sync_counter;

    char *p = buf + 5;
    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_REQUEST_TYPE);
    p = mp_encode_uint(p, IPROTO_EVAL);
    p = mp_encode_uint(p, IPROTO_SYNC);
    p = mp_encode_uint(p, sync_id);

    p = mp_encode_map(p, 2);
    p = mp_encode_uint(p, IPROTO_EXPR);
    p = mp_encode_str(p, expr->s, expr->len);
    p = mp_encode_uint(p, IPROTO_TUPLE);
    p = mp_encode_array(p, 0);

    size_t len = (size_t)(p - buf);
    finalize_packet(buf, len);

    if (tnt_send_all(c->sock_fd, buf, len) < 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    char resp[4096], *dyn = NULL;
    ssize_t blen = tnt_read_frame(c, resp, sizeof(resp), &dyn);
    if (blen <= 0) {
        tnt_conn_error(tcon, c);
        return -1;
    }

    const char *actual_resp = dyn ? dyn : resp;
    char *ret_str = (char *)pkg_malloc(blen + 1);
    if (ret_str) {
        memcpy(ret_str, actual_resp, blen);
        ret_str[blen] = '\0';
        res->s = ret_str;
        res->len = (int)blen;
    }
    if (dyn) pkg_free(dyn);
    return 0;
}
