#include "winject_proxy.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "ota_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "winject_proxy";

// d-plane registrations are 1-23 bytes (shorter than an 802.11 header).
#define REGISTRATION_MAX 23
#define DATAGRAM_MAX 1500

static winject_proxy_config_t s_cfg;
static int s_mplane_fd = -1;
static int s_dplane_fd = -1;

// Reply / forward destinations. Written by the UDP tasks, read by the link
// receive task; a torn read only misdirects one datagram during a change.
static struct sockaddr_in s_mplane_peer;
static atomic_bool s_mplane_peer_set;
static struct sockaddr_in s_dplane_peer;
static atomic_bool s_dplane_peer_set;

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static winject_proxy_stats_t s_stats;

#define STAT_INC(field)                   \
    do {                                  \
        taskENTER_CRITICAL(&s_stats_mux); \
        s_stats.field++;                  \
        taskEXIT_CRITICAL(&s_stats_mux);  \
    } while (0)

static int bind_udp(uint16_t port, int rcvbuf)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "bind %u: %d", port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

// Link receive task: a message from the C6.
static void on_link_message(void *ctx, uint8_t plane, const uint8_t *payload, uint16_t len)
{
    if (plane == SDIO_MSG_PLANE_OTA) {
        ota_client_on_reply(payload, len);
        return;
    }
    if (plane == SDIO_MSG_PLANE_MPLANE) {
        if (!atomic_load(&s_mplane_peer_set)) {
            STAT_INC(mplane_dropped);
            return;
        }
        struct sockaddr_in to = s_mplane_peer;
        if (sendto(s_mplane_fd, payload, len, 0, (struct sockaddr *)&to, sizeof(to)) == len) {
            STAT_INC(mplane_replies);
        } else {
            STAT_INC(mplane_dropped);
        }
        return;
    }
    if (!atomic_load(&s_dplane_peer_set)) {
        STAT_INC(dplane_up_no_peer);
        return;
    }
    struct sockaddr_in to = s_dplane_peer;
    if (sendto(s_dplane_fd, payload, len, 0, (struct sockaddr *)&to, sizeof(to)) == len) {
        STAT_INC(dplane_up);
    } else {
        STAT_INC(dplane_up_failed);
    }
}

static void mplane_task(void *arg)
{
    static uint8_t buf[DATAGRAM_MAX];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_mplane_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        ota_client_confirm_p4_image();
        s_mplane_peer = from;
        atomic_store(&s_mplane_peer_set, true);
        if (sdio_link_host_send(SDIO_MSG_PLANE_MPLANE, buf, n, 1000) == ESP_OK) {
            STAT_INC(mplane_requests);
        } else {
            STAT_INC(mplane_dropped);
        }
    }
}

static void dplane_task(void *arg)
{
    static uint8_t buf[DATAGRAM_MAX];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_dplane_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        ota_client_confirm_p4_image();
        if (n <= REGISTRATION_MAX) {
            if (!atomic_load(&s_dplane_peer_set) || s_dplane_peer.sin_addr.s_addr != from.sin_addr.s_addr ||
                s_dplane_peer.sin_port != from.sin_port) {
                char host[16];
                inet_ntoa_r(from.sin_addr, host, sizeof(host));
                ESP_LOGI(TAG, "d-plane peer %s:%u", host, ntohs(from.sin_port));
            }
            s_dplane_peer = from;
            atomic_store(&s_dplane_peer_set, true);
        }
        // Registrations go to the C6 too: it forwards nothing until it has one.
        if (sdio_link_host_send(SDIO_MSG_PLANE_DPLANE, buf, n, s_cfg.dplane_send_timeout_ms) == ESP_OK) {
            STAT_INC(dplane_down);
        } else {
            STAT_INC(dplane_down_dropped);
        }
    }
}

esp_err_t winject_proxy_start(const winject_proxy_config_t *config)
{
    s_cfg = *config;
    s_mplane_fd = bind_udp(config->mplane_port, 16 * 1024);
    s_dplane_fd = bind_udp(config->dplane_port, 64 * 1024);
    if (s_mplane_fd < 0 || s_dplane_fd < 0) {
        return ESP_FAIL;
    }
    if (config->http_port) {
        esp_err_t herr = ota_client_start_http(config->http_port);
        if (herr != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server: 0x%x", herr);
        }
    }
    esp_err_t err = sdio_link_host_start(&config->link, on_link_message, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDIO link: 0x%x", err);
        return err;
    }
    if (xTaskCreate(mplane_task, "proxy_mplane", 4096, NULL, config->mplane_task_prio, NULL) != pdPASS ||
        xTaskCreate(dplane_task, "proxy_dplane", 4096, NULL, config->dplane_task_prio, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UDP %u (m-plane) and %u (d-plane) <-> SDIO", config->mplane_port, config->dplane_port);
    return ESP_OK;
}

void winject_proxy_get_stats(winject_proxy_stats_t *out)
{
    taskENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    taskEXIT_CRITICAL(&s_stats_mux);
}
