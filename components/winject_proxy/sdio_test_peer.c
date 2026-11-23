#include "sdio_test_peer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdio_link_host.h"
#include "sdio_msg.h"
#include "sdio_test.h"
#include "sdio_test_meter.h"

static const char *TAG = "sdio_test";

#define QUEUE_LEN 4
#define TASK_STACK 4096
#define SEND_TIMEOUT_MS 1000

// Control messages from the C6, handled on the peer task.
typedef union {
    uint8_t type;
    sdio_test_bw_tx_t bw_tx;
    sdio_test_get_rx_stats_t get;
} control_t;

static QueueHandle_t s_queue;
static atomic_bool s_stop_tx;

// C6 -> P4 test packets (link receive task writes, peer task reads / resets).
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static sdio_test_rx_t s_rx;

// Current window (peer task only).
static sdio_test_meter_mark_t s_mark;
static sdio_link_host_stats_t s_link0;

void sdio_test_peer_on_message(const uint8_t *payload, uint16_t len)
{
    if (len == 0) {
        return;
    }
    const uint8_t type = payload[0];
    if (type == SDIO_TEST_PACKET) {
        const int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_rx_mux);
        sdio_test_rx_packet(&s_rx, payload, len, now);
        taskEXIT_CRITICAL(&s_rx_mux);
        return;
    }
    control_t c;
    if (type == SDIO_TEST_BW_TX && len >= sizeof(c.bw_tx)) {
        memcpy(&c.bw_tx, payload, sizeof(c.bw_tx));
        if (c.bw_tx.count == 0) {
            // Stop: the peer task is busy sending, so flag it from here.
            atomic_store(&s_stop_tx, true);
            return;
        }
        // In order with the stops: one that follows this start counts.
        atomic_store(&s_stop_tx, false);
    } else if (type == SDIO_TEST_GET_RX_STATS && len >= sizeof(c.get)) {
        memcpy(&c.get, payload, sizeof(c.get));
    } else {
        return;
    }
    // Never block the link receive task; a full queue drops the request and
    // the C6 times out.
    if (s_queue == NULL || xQueueSend(s_queue, &c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "request 0x%02x dropped", type);
    }
}

static void send_reply(const void *msg, size_t len)
{
    if (sdio_link_host_send(SDIO_MSG_PLANE_TEST, msg, (uint16_t)len, SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "reply 0x%02x not sent", ((const uint8_t *)msg)[0]);
    }
}

// Spaces sends interval_us apart: whole ticks asleep, the rest busy; after a
// stall it restarts from now rather than bursting.
static void pace(int64_t *next_us, uint32_t interval_us)
{
    if (interval_us == 0) {
        return;
    }
    const int64_t tick_us = 1000 * portTICK_PERIOD_MS;
    int64_t now = esp_timer_get_time();
    while (*next_us > now) {
        const int64_t ahead = *next_us - now;
        if (ahead >= tick_us) {
            vTaskDelay((TickType_t)(ahead / tick_us));
        } else {
            esp_rom_delay_us((uint32_t)ahead);
        }
        now = esp_timer_get_time();
    }
    *next_us += interval_us;
    if (*next_us < now) {
        *next_us = now;
    }
}

static void run_bw_tx(const sdio_test_bw_tx_t *req)
{
    sdio_test_bw_tx_done_t done = {.type = SDIO_TEST_BW_TX_DONE, .run = req->run};
    const uint16_t mtu = req->mtu;
    uint8_t *buf = (mtu >= sizeof(sdio_test_packet_t) && mtu <= SDIO_MSG_MAX_PAYLOAD) ? malloc(mtu) : NULL;
    if (buf == NULL) {
        done.send_fail = req->count;
        send_reply(&done, sizeof(done));
        return;
    }
    for (uint16_t i = 0; i < mtu; i++) {
        buf[i] = (uint8_t)(i * 13 + 5);
    }
    const int64_t t0 = esp_timer_get_time();
    int64_t next = t0;
    for (uint32_t seq = 0; seq < req->count && !atomic_load(&s_stop_tx); seq++) {
        pace(&next, req->pacing_us);
        sdio_test_packet_set(buf, req->run, seq);
        if (sdio_link_host_send(SDIO_MSG_PLANE_TEST, buf, mtu, SEND_TIMEOUT_MS) == ESP_OK) {
            done.sent++;
        } else {
            done.send_fail++;
        }
    }
    done.elapsed_us = (uint32_t)(esp_timer_get_time() - t0);
    free(buf);
    send_reply(&done, sizeof(done));
    ESP_LOGI(TAG, "bw_tx run %u: sent %lu fail %lu in %lu us", req->run, (unsigned long)done.sent,
             (unsigned long)done.send_fail, (unsigned long)done.elapsed_us);
}

static void answer_get(const sdio_test_get_rx_stats_t *req)
{
    sdio_test_rx_stats_t r = {.type = SDIO_TEST_RX_STATS, .run = req->run};
    taskENTER_CRITICAL(&s_rx_mux);
    r.rx = s_rx.c;
    taskEXIT_CRITICAL(&s_rx_mux);
    sdio_test_meter_read(&s_mark, &r.window);
    sdio_link_host_stats_t ls;
    sdio_link_host_get_stats(&ls);
    r.link_credit_waits = ls.tx_credit_waits - s_link0.tx_credit_waits;
    r.link_rx_reads = ls.rx_reads - s_link0.rx_reads;
    r.link_tx_writes = ls.tx_writes - s_link0.tx_writes;
    r.link_tx_slots = ls.tx_slots - s_link0.tx_slots;
    r.link_tx_msgs = ls.tx_msgs - s_link0.tx_msgs;
    r.link_freq_khz = (uint32_t)ls.real_freq_khz;
    r.link_downs = ls.link_downs - s_link0.link_downs;
    if (req->flags & SDIO_TEST_FLAG_CLEAR) {
        // New window before the reply: the C6 starts sending once it has it.
        esp_err_t err = sdio_test_probe_set(req->probe);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "probe %u: %s", req->probe, esp_err_to_name(err));
        }
        // Optional: no monitor below P4 rev 3.0; the window then has no AXI figures.
        err = sdio_test_axi_set(req->axi_agent);
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "AXI monitor: %s", esp_err_to_name(err));
        }
        taskENTER_CRITICAL(&s_rx_mux);
        sdio_test_rx_reset(&s_rx, req->run);
        taskEXIT_CRITICAL(&s_rx_mux);
        sdio_link_host_get_stats(&s_link0);
        sdio_test_meter_mark(&s_mark);
    }
    send_reply(&r, sizeof(r));
}

static void peer_task(void *arg)
{
    for (;;) {
        control_t c;
        if (xQueueReceive(s_queue, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (c.type == SDIO_TEST_BW_TX) {
            run_bw_tx(&c.bw_tx);
        } else {
            answer_get(&c.get);
        }
    }
}

esp_err_t sdio_test_peer_start(int task_prio)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    sdio_test_meter_mark(&s_mark);
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(control_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(peer_task, "sdio_test", TASK_STACK, NULL, task_prio, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
