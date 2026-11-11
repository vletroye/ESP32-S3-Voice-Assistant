/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Games between BOX: the UDP messages between the BOX of the network (see game_internal.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "game.h"
#include "game_internal.h"

static const char *TAG = "game";

/*
 * Each message sent this many times, GAME_SEND_GAP_MS apart (UDP broadcast: a copy may be lost, two sent at once
 * were lost together; duplicates are dropped by the turn)
 */
#define GAME_SEND_TIMES     3
#define GAME_SEND_GAP_MS    40

/* main.c */
extern void assistant_save_soon(void);

static int s_sock = -1;
static QueueHandle_t s_queue = NULL;
static uint64_t s_my_id = 0;

/*
 * The other BOX heard on the network and their address: each message also goes to each of them directly. A
 * broadcast is neither acknowledged nor sent again by the Wi-Fi: a BOX listening (its audio sent to the
 * transcription) missed every "done" broadcast, a direct packet is acknowledged and repeated by the Wi-Fi.
 */
#define GAME_PEERS_MAX      8
typedef struct {
    uint64_t id;
    uint32_t ip;            /* network order */
} peer_t;
static peer_t s_peers[GAME_PEERS_MAX];
static volatile int s_peer_count = 0;

/* From the network task: a BOX and its address, kept (the newest address) */
static void peer_seen(uint64_t id, uint32_t ip)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].id == id) {
            s_peers[i].ip = ip;
            return;
        }
    }
    if (s_peer_count < GAME_PEERS_MAX) {
        s_peers[s_peer_count].id = id;
        s_peers[s_peer_count].ip = ip;
        s_peer_count++;
        ESP_LOGI(TAG, "BOX %llx at %s", (unsigned long long)id, inet_ntoa(*(struct in_addr *)&ip));
    }
}

uint64_t game_net_id(void)
{
    return s_my_id;
}

static void send_to(uint32_t ip, cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    if (!text || s_sock < 0) {
        free(text);
        return;
    }
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(GAME_PORT),
        .sin_addr.s_addr = ip,
    };
    for (int i = 0; i < GAME_SEND_TIMES; i++) {
        if (i) {
            vTaskDelay(pdMS_TO_TICKS(GAME_SEND_GAP_MS));
        }
        sendto(s_sock, text, strlen(text), 0, (struct sockaddr *)&to, sizeof(to));
    }
    free(text);
}

cJSON *game_net_msg(const char *type, uint32_t nonce)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", type);
    char id[20];
    snprintf(id, sizeof(id), "%llx", (unsigned long long)s_my_id);
    cJSON_AddStringToObject(m, "id", id);
    cJSON_AddNumberToObject(m, "n", nonce);
    return m;
}

void game_net_send(cJSON *msg)
{
    // To each BOX known, directly (acknowledged by the Wi-Fi), then to all (the BOX not known yet)
    for (int i = 0; i < s_peer_count; i++) {
        send_to(s_peers[i].ip, msg);
    }
    send_to(htonl(INADDR_BROADCAST), msg);
    cJSON_Delete(msg);
}

int64_t game_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool game_net_post(const game_msg_t *msg)
{
    if (!s_queue || xQueueSend(s_queue, msg, 0) != pdTRUE) {
        return false;
    }
    assistant_save_soon();      // the main task, at once
    return true;
}

bool game_net_receive(game_msg_t *msg)
{
    return s_queue && xQueueReceive(s_queue, msg, 0) == pdTRUE;
}

static void net_task(void *arg)
{
    char *buf = heap_caps_malloc(GAME_TEXT_MAX + 512, MALLOC_CAP_SPIRAM);
    for (;;) {
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int n = buf ? recvfrom(s_sock, buf, GAME_TEXT_MAX + 511, 0, (struct sockaddr *)&from, &len) : -1;
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        buf[n] = '\0';
        cJSON *json = cJSON_Parse(buf);
        const cJSON *type = cJSON_GetObjectItem(json, "t");
        const cJSON *id = cJSON_GetObjectItem(json, "id");
        uint64_t sender = cJSON_IsString(id) ? strtoull(id->valuestring, NULL, 16) : 0;
        if (cJSON_IsString(type) && sender && sender != s_my_id) {
            peer_seen(sender, from.sin_addr.s_addr);
            // Each message once in the log (its copies not): to tell a message lost on the way from one not taken
            static char s_last[40];
            const cJSON *t = cJSON_GetObjectItem(json, "turn");
            char key[40];
            snprintf(key, sizeof(key), "%.8s %llx %d", type->valuestring, (unsigned long long)sender,
                     cJSON_IsNumber(t) ? t->valueint : -1);
            if (strcmp(key, s_last) != 0) {
                strlcpy(s_last, key, sizeof(s_last));
                ESP_LOGI(TAG, "net: %s, line %d, %d bytes", type->valuestring, cJSON_IsNumber(t) ? t->valueint : -1, n);
            }
            game_msg_t msg = { 0 };
            strlcpy(msg.type, type->valuestring, sizeof(msg.type));
            msg.from = sender;
            const cJSON *v = cJSON_GetObjectItem(json, "n");
            msg.nonce = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
            v = cJSON_GetObjectItem(json, "turn");
            msg.turn = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(json, "round");
            msg.round = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(json, "ts");
            msg.ts = cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
            v = cJSON_GetObjectItem(json, "max");
            msg.turns_max = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(json, "tone");
            msg.tone = cJSON_IsNumber(v) ? v->valueint : -1;
            v = cJSON_GetObjectItem(json, "game");
            strlcpy(msg.game, cJSON_IsString(v) ? v->valuestring : "", sizeof(msg.game));
            v = cJSON_GetObjectItem(json, "tag");
            strlcpy(msg.tag, cJSON_IsString(v) ? v->valuestring : "", sizeof(msg.tag));
            v = cJSON_GetObjectItem(json, "text");
            msg.text = cJSON_IsString(v) ? strdup(v->valuestring) : NULL;
            if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
                ESP_LOGW(TAG, "message \"%s\" (line %d) lost: the queue is full", msg.type, msg.turn);
                free(msg.text);
            } else {
                assistant_save_soon();      // the main task, at once
            }
        }
        cJSON_Delete(json);
    }
}

bool game_net_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    for (int i = 0; i < 6; i++) {
        s_my_id = (s_my_id << 8) | mac[i];
    }
    // In PSRAM, as its task's stack: the internal RAM is short, TLS needs it for the calls to the services
    s_queue = xQueueCreateWithCaps(16, sizeof(game_msg_t), MALLOC_CAP_SPIRAM);
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(GAME_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (!s_queue || s_sock < 0 || bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "UDP port %d not opened", GAME_PORT);
        return false;
    }
    // Small: it only reads the messages and gives them to the main task. Above the listening's tasks (5, on core 0
    // too: a BOX listening never got the "done" of the one speaking); it sleeps until a message comes
    if (xTaskCreatePinnedToCoreWithCaps(net_task, "game_net", 3072, NULL, 7, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        return false;
    }
    return true;
}
