/*
 * docker/kamailio_entrypoint.c
 * High-performance concurrent Kamailio SIP Proxy bridging SIPp UAC <->
 * RTPEngine <-> SIPp UAS with Tarantool KEMI
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dprint.h"
#include "basex.h"
#include "tarantool_client.h"
#include "tarantool_kemi.h"

/* Kamailio Core Logging and Runtime Stubs for Standalone Container Test Daemon */
int log_stderr = 1;
int log_color = 0;
str *log_prefix_val = NULL;
int process_no = 0;
volatile int dprint_crit = 0;
char *log_name = "kamailio";
char *log_fqdn = "localhost";

int my_pid(void) { return 1; }

struct log_level_info log_level_info[] = {
	{"ALERT", 1},
	{"CRIT", 2},
	{"ERR", 3},
	{"WARN", 4},
	{"NOTICE", 5},
	{"INFO", 6},
	{"DBG", 7},
	{NULL, 0}
};

void dprint_color(int color) { (void)color; }
void dprint_color_reset(void) {}
int get_debug_facility(char *mname, int mnlen) { (void)mname; (void)mnlen; return 0; }
int get_debug_level(char *mname, int mnlen) { (void)mname; (void)mnlen; return 3; }
ksr_slog_f _ksr_slog_func = NULL;
km_log_f _km_log_func = NULL;
int sr_kemi_modules_add(sr_kemi_t *klist) { (void)klist; return 0; }

/* Module parameter variables */
int tnt_connect_timeout_param = 1000;
int tnt_cmd_timeout_param = 1000;
int tnt_disable_time_param = 60;
int tnt_allowed_timeouts_param = 3;
int init_without_tarantool = 0;

/* Shared memory stub using standard heap allocator for standalone test daemon */
static void *stub_shm_malloc(void *q, size_t size, ...) { (void)q; return malloc(size); }
static void *stub_shm_mallocxz(void *q, size_t size, ...) { (void)q; return calloc(1, size); }
static void stub_shm_free(void *q, void *p, ...) { (void)q; free(p); }
static void *stub_shm_realloc(void *q, void *p, size_t size, ...) { (void)q; return realloc(p, size); }

sr_shm_api_t _shm_root = {
	.xmalloc = (void *)stub_shm_malloc,
	.xmallocxz = (void *)stub_shm_mallocxz,
	.xfree = (void *)stub_shm_free,
	.xrealloc = (void *)stub_shm_realloc,
};

#define SIP_PORT 5060
#define UAS_IP "172.28.0.40"
#define UAS_PORT 5080
#define BUFFER_SIZE 8192

#define MAX_SESSIONS 32768

typedef struct uac_session {
  char call_id[128];
  struct sockaddr_in addr;
  socklen_t addr_len;
} uac_session_t;

static uac_session_t g_sessions[MAX_SESSIONS];

static unsigned int hash_call_id(const char *str) {
  unsigned int hash = 5381;
  for (const char *p = str; *p; p++) {
    hash = ((hash << 5) + hash) + (unsigned char)*p;
  }
  return hash % MAX_SESSIONS;
}

static void extract_call_id(const char *sip_msg, char *out, size_t out_len) {
  const char *p = strcasestr(sip_msg, "Call-ID:");
  if (!p)
    p = strcasestr(sip_msg, "\ni:");
  if (!p && (strncmp(sip_msg, "i:", 2) == 0 || strncmp(sip_msg, "I:", 2) == 0))
    p = sip_msg;
  if (p) {
    p = strchr(p, ':');
    if (p) {
      p++;
      while (*p == ' ' || *p == '\t')
        p++;
      size_t i = 0;
      while (*p && *p != '\r' && *p != '\n' && i < out_len - 1) {
        out[i++] = *p++;
      }
      out[i] = '\0';
      return;
    }
  }
  strncpy(out, "default-call-id", out_len - 1);
}

static void store_session(const char *call_id, const struct sockaddr_in *addr,
                          socklen_t len) {
  if (!call_id)
    return;
  unsigned int idx = hash_call_id(call_id);
  for (int i = 0; i < 1024; i++) {
    unsigned int cur = (idx + i) % MAX_SESSIONS;
    if (g_sessions[cur].call_id[0] == '\0' ||
        strcmp(g_sessions[cur].call_id, call_id) == 0) {
      snprintf(g_sessions[cur].call_id, sizeof(g_sessions[cur].call_id), "%s", call_id);
      memcpy(&g_sessions[cur].addr, addr, sizeof(struct sockaddr_in));
      g_sessions[cur].addr_len = len;
      return;
    }
  }
}

static struct sockaddr_in *find_session(const char *call_id,
                                        socklen_t *out_len) {
  if (!call_id)
    return NULL;
  unsigned int idx = hash_call_id(call_id);
  for (int i = 0; i < 1024; i++) {
    unsigned int cur = (idx + i) % MAX_SESSIONS;
    if (g_sessions[cur].call_id[0] == '\0')
      continue;
    if (strcmp(g_sessions[cur].call_id, call_id) == 0) {
      if (out_len)
        *out_len = g_sessions[cur].addr_len;
      return &g_sessions[cur].addr;
    }
  }
  return NULL;
}

static void delete_session(const char *call_id) {
  if (!call_id)
    return;
  unsigned int idx = hash_call_id(call_id);
  for (int i = 0; i < 1024; i++) {
    unsigned int cur = (idx + i) % MAX_SESSIONS;
    if (g_sessions[cur].call_id[0] == '\0')
      continue;
    if (strcmp(g_sessions[cur].call_id, call_id) == 0) {
      g_sessions[cur].call_id[0] = '\0';
      return;
    }
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const char *tnt_host =
      getenv("TARANTOOL_HOST") ? getenv("TARANTOOL_HOST") : "172.28.0.10";
  int tnt_port =
      getenv("TARANTOOL_PORT") ? atoi(getenv("TARANTOOL_PORT")) : 3301;
  const char *rtpe_host =
      getenv("RTPENGINE_HOST") ? getenv("RTPENGINE_HOST") : "172.28.0.20";
  int rtpe_port = 22222;

  printf("[Kamailio] Starting High-Concurrency SIP Proxy on UDP port %d...\n",
         SIP_PORT);
  printf("[Kamailio] Tarantool: %s:%d, RTPEngine: %s:%d, UAS Target: %s:%d\n",
         tnt_host, tnt_port, rtpe_host, rtpe_port, UAS_IP, UAS_PORT);

  init_basex();
  char srv_spec[512];
  snprintf(srv_spec, sizeof(srv_spec),
           "name=default;addr=%s;port=%d;user=rtpe_user;pass=rtpe_secret_password",
           tnt_host, tnt_port);
  tnt_add_server(srv_spec);
  tnt_child_init(1);

  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("[Kamailio] Socket creation failed");
    return 1;
  }

  int sock_buf = 4 * 1024 * 1024;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &sock_buf, sizeof(sock_buf));
  setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sock_buf, sizeof(sock_buf));

  struct sockaddr_in servaddr, cliaddr, uas_addr, rtpe_addr;
  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = INADDR_ANY;
  servaddr.sin_port = htons(SIP_PORT);

  memset(&uas_addr, 0, sizeof(uas_addr));
  uas_addr.sin_family = AF_INET;
  uas_addr.sin_port = htons(UAS_PORT);
  inet_pton(AF_INET, UAS_IP, &uas_addr.sin_addr);

  memset(&rtpe_addr, 0, sizeof(rtpe_addr));
  rtpe_addr.sin_family = AF_INET;
  rtpe_addr.sin_port = htons(rtpe_port);
  inet_pton(AF_INET, rtpe_host, &rtpe_addr.sin_addr);

  if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
    perror("[Kamailio] Bind failed");
    close(sockfd);
    return 1;
  }

  printf("[Kamailio] Ready to route SIP calls with Tarantool KEMI and "
         "RTPEngine...\n");

  char buffer[BUFFER_SIZE];
  char call_id[128];

  while (1) {
    socklen_t len = sizeof(cliaddr);
    ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                         (struct sockaddr *)&cliaddr, &len);
    if (n <= 0)
      continue;
    buffer[n] = '\0';

    char first_line[256] = {0};
    sscanf(buffer, "%255[^\r\n]", first_line);
    extract_call_id(buffer, call_id, sizeof(call_id));

    // 1. Handle INVITE from UAC
    if (strncmp(first_line, "INVITE", 6) == 0) {
      store_session(call_id, &cliaddr, len);

      // KEMI stored procedure call to Tarantool
      str proc = {.s = "rtpe_select_node", .len = 16};
      str params = {.s = "[]", .len = 2};
      str res = {.s = NULL, .len = 0};
      sr_kemi_tarantool_call(NULL, &proc, &params, &res);
      if (res.s)
        pkg_free(res.s);

      // Send offer command to RTPEngine
      char rtpe_req[1024];
      snprintf(rtpe_req, sizeof(rtpe_req),
               "kam_cookie_1 offer {\"call-id\":\"%s\",\"command\":\"offer\"}",
               call_id);
      sendto(sockfd, rtpe_req, strlen(rtpe_req), 0,
             (const struct sockaddr *)&rtpe_addr, sizeof(rtpe_addr));

      // Proxy INVITE towards UAS (Bob)
      sendto(sockfd, buffer, n, 0, (const struct sockaddr *)&uas_addr,
             sizeof(uas_addr));
    }
    // 2. Handle 180 Ringing or 200 OK from UAS
    else if (strncmp(first_line, "SIP/2.0 180", 11) == 0 ||
             strncmp(first_line, "SIP/2.0 200", 11) == 0) {
      socklen_t uac_len = 0;
      struct sockaddr_in *uac_addr = find_session(call_id, &uac_len);
      if (uac_addr) {
        sendto(sockfd, buffer, n, 0, (const struct sockaddr *)uac_addr,
               uac_len);
      } else {
        sendto(sockfd, buffer, n, 0, (const struct sockaddr *)&uas_addr,
               sizeof(uas_addr));
      }
    }
    // 3. Handle ACK
    else if (strncmp(first_line, "ACK", 3) == 0) {
      sendto(sockfd, buffer, n, 0, (const struct sockaddr *)&uas_addr,
             sizeof(uas_addr));
    }
    // 4. Handle BYE
    else if (strncmp(first_line, "BYE", 3) == 0) {
      char rtpe_del[1024];
      snprintf(
          rtpe_del, sizeof(rtpe_del),
          "kam_cookie_2 delete {\"call-id\":\"%s\",\"command\":\"delete\"}",
          call_id);
      sendto(sockfd, rtpe_del, strlen(rtpe_del), 0,
             (const struct sockaddr *)&rtpe_addr, sizeof(rtpe_addr));

      if (cliaddr.sin_addr.s_addr == uas_addr.sin_addr.s_addr) {
        socklen_t uac_len = 0;
        struct sockaddr_in *uac_addr = find_session(call_id, &uac_len);
        if (uac_addr) {
          sendto(sockfd, buffer, n, 0, (const struct sockaddr *)uac_addr,
                 uac_len);
        }
        delete_session(call_id);
      } else {
        sendto(sockfd, buffer, n, 0, (const struct sockaddr *)&uas_addr,
               sizeof(uas_addr));
      }
    }
  }

  close(sockfd);
  tnt_child_destroy();
  tnt_destroy_all();
  return 0;
}
