/*
 * docker/kamailio_entrypoint.c
 * High-performance concurrent Kamailio SIP Proxy bridging SIPp UAC <->
 * RTPEngine <-> SIPp UAS with Tarantool KEMI
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tarantool_client.h"
#include "tarantool_kemi.h"

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

extern tnt_pool_t tnt_global_pool;

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

  tnt_pool_init(&tnt_global_pool, tnt_host, tnt_port, "rtpe_user",
                "rtpe_secret_password", 8, 500);

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
      str_t proc = {.s = "rtpe_select_node", .len = 16};
      str_t params = {.s = "[]", .len = 2};
      str_t res = {.s = NULL, .len = 0};
      sr_kemi_tarantool_call(NULL, &proc, &params, &res);
      if (res.s)
        free(res.s);

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
  tnt_pool_destroy(&tnt_global_pool);
  return 0;
}
