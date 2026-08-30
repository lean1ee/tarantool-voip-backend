/*
 * docker/rtpengine_entrypoint.c
 * Standalone RTPEngine Daemon Runner integrating rtpengine_tarantool driver
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#include "tarantool.h"

#define NG_PORT 22222
#define BUFFER_SIZE 4096

int main(int argc, char **argv) {
    const char *tnt_host = getenv("TARANTOOL_HOST") ? getenv("TARANTOOL_HOST") : "127.0.0.1";
    int tnt_port = getenv("TARANTOOL_PORT") ? atoi(getenv("TARANTOOL_PORT")) : 3301;
    const char *node_id = getenv("NODE_ID") ? getenv("NODE_ID") : "rtpe-node-01";

    printf("[RTPEngine-Tarantool] Starting daemon on UDP port %d (node: %s)...\n", NG_PORT, node_id);
    printf("[RTPEngine-Tarantool] Connecting to Tarantool at %s:%d...\n", tnt_host, tnt_port);

    rtpe_tarantool_client_t *tnt_client = rtpe_tarantool_new(tnt_host, tnt_port, "rtpe_user", "rtpe_secret_password", node_id);
    if (!tnt_client) {
        fprintf(stderr, "[RTPEngine-Tarantool] Failed to allocate Tarantool client\n");
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("[RTPEngine] Socket creation failed");
        return 1;
    }

    struct sockaddr_in servaddr, cliaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(NG_PORT);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("[RTPEngine] Socket bind failed");
        close(sockfd);
        return 1;
    }

    printf("[RTPEngine-Tarantool] Ready and listening for NG commands...\n");

    char buffer[BUFFER_SIZE];
    while (1) {
        socklen_t len = sizeof(cliaddr);
        ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&cliaddr, &len);
        if (n <= 0) continue;
        buffer[n] = '\0';

        printf("[RTPEngine NG] Received packet: %s\n", buffer);

        // Parse NG command (e.g. "cookie offer {call-id: ...}")
        char cookie[64] = {0};
        char command[32] = {0};
        sscanf(buffer, "%63s %31s", cookie, command);

        char response[BUFFER_SIZE];
        if (strcmp(command, "offer") == 0 || strcmp(command, "answer") == 0) {
            // Save call session to Tarantool backend
            char call_id[128] = "sample-call-id-12345";
            char *cid_pos = strstr(buffer, "call-id");
            if (cid_pos) {
                sscanf(cid_pos, "call-id\":\"%127[^\"]", call_id);
            }

            rtpe_call_info_t info = {
                .call_id = call_id,
                .call_id_len = strlen(call_id),
                .node_id = node_id,
                .caller_ip = "172.28.0.100",
                .caller_port = 30002,
                .callee_ip = "172.28.0.101",
                .callee_port = 30004,
                .srtp_suite = "AES_CM_128_HMAC_SHA1_80",
                .ttl_sec = 120
            };

            int rc = rtpe_tarantool_save_call(tnt_client, &info);
            printf("[RTPEngine-Tarantool] Call '%s' saved to Tarantool (rc=%d)\n", call_id, rc);

            snprintf(response, sizeof(response), "%s d8:result2:ok4:porti30002ee", cookie);
        } else if (strcmp(command, "delete") == 0) {
            char call_id[128] = "sample-call-id-12345";
            rtpe_tarantool_delete_call(tnt_client, call_id, strlen(call_id));
            printf("[RTPEngine-Tarantool] Call '%s' deleted from Tarantool\n", call_id);

            snprintf(response, sizeof(response), "%s d8:result2:okee", cookie);
        } else if (strcmp(command, "ping") == 0) {
            snprintf(response, sizeof(response), "%s d8:result4:pongee", cookie);
        } else {
            snprintf(response, sizeof(response), "%s d8:result2:okee", cookie);
        }

        sendto(sockfd, response, strlen(response), 0, (const struct sockaddr *)&cliaddr, len);
    }

    close(sockfd);
    rtpe_tarantool_free(tnt_client);
    return 0;
}
