// HTTP firmware update of the C6 through the SDIO OTA plane (sdio_ota.h).

#include "ota_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdio_link_host.h"
#include "sdio_ota.h"

static const char *TAG = "proxy_ota";

#define TRIES 3

static QueueHandle_t s_replies;  // sdio_ota_reply_t from the C6
static SemaphoreHandle_t s_busy;
static uint8_t s_msg[sizeof(sdio_ota_req_t) + SDIO_OTA_CHUNK_MAX];

void ota_client_on_reply(const uint8_t *payload, uint16_t len)
{
    sdio_ota_reply_t r;
    if (s_replies == NULL || len < sizeof(r)) {
        return;
    }
    memcpy(&r, payload, sizeof(r));
    xQueueSend(s_replies, &r, 0);
}

// One request, retried on a lost request or reply. DATA repeats are safe: the
// C6 acknowledges an offset it already wrote without writing it again.
static esp_err_t request(uint8_t op, uint32_t arg, const uint8_t *body, uint16_t body_len,
                         uint32_t timeout_ms, sdio_ota_reply_t *out)
{
    sdio_ota_req_t req = {.op = op, .arg = arg};
    memcpy(s_msg, &req, sizeof(req));
    if (body_len) {
        memcpy(s_msg + sizeof(req), body, body_len);
    }
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (int t = 0; t < TRIES; t++) {
        xQueueReset(s_replies);
        err = sdio_link_host_send(SDIO_MSG_PLANE_OTA, s_msg, sizeof(req) + body_len, 1000);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
        while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
            if (xQueueReceive(s_replies, out, pdMS_TO_TICKS(50)) == pdTRUE && out->op == op) {
                return out->status == SDIO_OTA_STATUS_OK ? ESP_OK : (esp_err_t)out->err;
            }
        }
        err = ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "op %u arg %u: no reply, retry %d", op, (unsigned)arg, t + 1);
    }
    return err;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *fmt, esp_err_t err)
{
    char msg[160];
    snprintf(msg, sizeof(msg), fmt, esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t update_c6(httpd_req_t *req)
{
    ota_client_confirm_p4_image();
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        return fail(req, "409 Conflict", "ERR an update is already running (%s)\n", ESP_ERR_INVALID_STATE);
    }
    esp_err_t ret = ESP_OK;
    const size_t size = req->content_len;
    static uint8_t chunk[SDIO_OTA_CHUNK_MAX];
    sdio_ota_reply_t r;
    int64_t t0 = esp_log_timestamp();

    if (size == 0) {
        ret = fail(req, "400 Bad Request", "ERR empty body: POST the image (%s)\n", ESP_ERR_INVALID_SIZE);
        goto out;
    }
    ESP_LOGI(TAG, "C6 update: %u B", (unsigned)size);
    esp_err_t err = request(SDIO_OTA_BEGIN, size, NULL, 0, 5000, &r);
    if (err != ESP_OK) {
        ret = fail(req, "500 Internal Server Error", "ERR begin: %s\n", err);
        goto out;
    }
    for (size_t off = 0; off < size;) {
        // Fill a whole chunk (the socket hands out arbitrary pieces).
        size_t n = 0;
        const size_t want = size - off < sizeof(chunk) ? size - off : sizeof(chunk);
        while (n < want) {
            const int got = httpd_req_recv(req, (char *)chunk + n, want - n);
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (got <= 0) {
                request(SDIO_OTA_ABORT, 0, NULL, 0, 1000, &r);
                ret = fail(req, "400 Bad Request", "ERR upload ended early (%s)\n", ESP_FAIL);
                goto out;
            }
            n += got;
        }
        err = request(SDIO_OTA_DATA, off, chunk, n, 3000, &r);
        if (err != ESP_OK || r.arg != off + n) {
            request(SDIO_OTA_ABORT, 0, NULL, 0, 1000, &r);
            ret = fail(req, "500 Internal Server Error", "ERR write: %s\n", err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE);
            goto out;
        }
        off += n;
    }
    // esp_ota_end reads the whole image back to check it.
    err = request(SDIO_OTA_END, 0, NULL, 0, 15000, &r);
    if (err != ESP_OK) {
        ret = fail(req, "500 Internal Server Error", "ERR image rejected by the C6: %s\n", err);
        goto out;
    }
    const int64_t t_written = esp_log_timestamp();

    // Boot the new image. Its first message from us confirms it (no rollback);
    // if it never answers, the link keeps resetting the C6 and the bootloader
    // falls back to the previous image.
    err = sdio_link_host_reset_c6(10000);
    if (err == ESP_OK) {
        err = request(SDIO_OTA_ABORT, 0, NULL, 0, 2000, &r);
    }
    if (err != ESP_OK) {
        // Reset once more: the bootloader sees the unconfirmed image and boots
        // the previous one.
        sdio_link_host_reset_c6(10000);
        ret = fail(req, "500 Internal Server Error",
                   "ERR image written but the C6 does not answer (%s); reset it so it boots the previous image\n",
                   err);
        goto out;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "OK C6 updated: %u B in %lld ms, restarted, new image answering\n",
             (unsigned)size, (long long)(t_written - t0));
    ESP_LOGI(TAG, "%s", msg);
    httpd_resp_sendstr(req, msg);
out:
    xSemaphoreGive(s_busy);
    return ret;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));  // let the HTTP reply go out
    esp_restart();
}

// POST /update/p4: write the raw image into the P4's inactive slot and boot it.
static esp_err_t update_p4(httpd_req_t *req)
{
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        return fail(req, "409 Conflict", "ERR an update is already running (%s)\n", ESP_ERR_INVALID_STATE);
    }
    esp_err_t ret = ESP_OK;
    const size_t size = req->content_len;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    static uint8_t buf[4096];
    const int64_t t0 = esp_log_timestamp();
    esp_err_t err;

    if (target == NULL || size == 0 || size > target->size) {
        ret = fail(req, "400 Bad Request", "ERR missing or oversized image (%s)\n", ESP_ERR_INVALID_SIZE);
        goto out;
    }
    err = esp_ota_begin(target, size, &h);
    if (err != ESP_OK) {
        ret = fail(req, "500 Internal Server Error", "ERR begin: %s\n", err);
        goto out;
    }
    for (size_t off = 0; off < size;) {
        const size_t want = size - off < sizeof(buf) ? size - off : sizeof(buf);
        const int got = httpd_req_recv(req, (char *)buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            esp_ota_abort(h);
            ret = fail(req, "400 Bad Request", "ERR upload ended early (%s)\n", ESP_FAIL);
            goto out;
        }
        err = esp_ota_write(h, buf, got);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            ret = fail(req, "500 Internal Server Error", "ERR write: %s\n", err);
            goto out;
        }
        off += got;
    }
    err = esp_ota_end(h);  // checks the image
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(target);
    }
    if (err != ESP_OK) {
        ret = fail(req, "500 Internal Server Error", "ERR image rejected: %s\n", err);
        goto out;
    }
    char msg[200];
    snprintf(msg, sizeof(msg),
             "OK P4 updated: %u B in %lld ms -> %s, restarting; it is kept once it is reached "
             "over the network (e.g. GET /), otherwise the next reset boots the previous image\n",
             (unsigned)size, (long long)(esp_log_timestamp() - t0), target->label);
    ESP_LOGI(TAG, "%s", msg);
    httpd_resp_sendstr(req, msg);
    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
out:
    xSemaphoreGive(s_busy);
    return ret;
}

void ota_client_confirm_p4_image(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new P4 image on %s confirmed (reached over the network)", running->label);
    }
}

static esp_err_t status(httpd_req_t *req)
{
    ota_client_confirm_p4_image();
    sdio_link_host_stats_t ls;
    sdio_link_host_get_stats(&ls);
    char msg[520];
    snprintf(msg, sizeof(msg),
             "winject-radio-esp32c6 P4 proxy\n"
             "link=%s clock_khz=%d downs=%u ups=%u rx_msgs=%u tx_msgs=%u\n"
             "\n"
             "Update firmware (raw image as the body):\n"
             "  curl --data-binary @c6/build/winject-esp32c6.bin http://<this address>/update/c6\n"
             "  curl --data-binary @p4/build/winject-p4-proxy.bin http://<this address>/update/p4\n",
             sdio_link_host_is_up() ? "up" : "down", ls.real_freq_khz, (unsigned)ls.link_downs,
             (unsigned)ls.link_ups, (unsigned)ls.rx_msgs, (unsigned)ls.tx_msgs);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

esp_err_t ota_client_start_http(uint16_t port)
{
    s_replies = xQueueCreate(2, sizeof(sdio_ota_reply_t));
    s_busy = xSemaphoreCreateMutex();
    if (s_replies == NULL || s_busy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t get_status = {.uri = "/", .method = HTTP_GET, .handler = status};
    const httpd_uri_t post_c6 = {.uri = "/update/c6", .method = HTTP_POST, .handler = update_c6};
    const httpd_uri_t post_p4 = {.uri = "/update/p4", .method = HTTP_POST, .handler = update_p4};
    httpd_register_uri_handler(server, &get_status);
    httpd_register_uri_handler(server, &post_c6);
    httpd_register_uri_handler(server, &post_p4);
    ESP_LOGI(TAG, "HTTP on port %u: GET /, POST /update/c6, POST /update/p4", port);
    return ESP_OK;
}
