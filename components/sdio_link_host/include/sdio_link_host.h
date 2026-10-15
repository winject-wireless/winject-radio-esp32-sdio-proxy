#pragma once

// P4 side of the SDIO link (docs/sdio-link.md): SDIO host for the C6 slave
// (sdio_link_slave). One receive task drains everything the C6 has queued per
// interrupt and hands each message to a callback.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdio_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int slot;            // SDMMC host slot
    int clk, cmd, d0, d1, d2, d3;  // pins (used where the slot routes through the GPIO matrix)
    int freq_khz;
    int reset_gpio;      // C6 CHIP_PU, pulsed low at start; -1 = none
    int rx_task_prio;
    int rx_task_stack;
    int boot_timeout_ms; // how long to wait for the C6's slave to answer after reset
} sdio_link_host_config_t;

// Waveshare ESP32-P4-WIFI6-DEV-KIT: slot 1, CLK18 CMD19 D0-D3 14-17, C6 EN on 54.
#define SDIO_LINK_HOST_CONFIG_DEFAULT()                                                     \
    {                                                                                       \
        .slot = 1, .clk = 18, .cmd = 19, .d0 = 14, .d1 = 15, .d2 = 16, .d3 = 17,            \
        .freq_khz = 20000, .reset_gpio = 54, .rx_task_prio = 20, .rx_task_stack = 4096,     \
        .boot_timeout_ms = 5000,                                                            \
    }

typedef struct {
    uint32_t rx_msgs, rx_bytes, rx_bad, rx_reads, rx_errors;
    uint32_t tx_msgs, tx_bytes, tx_errors;
    uint32_t tx_credit_waits;  // sends that found no free C6 buffer and slept a tick
    uint32_t link_downs, link_ups;  // C6 lost (reboot, reset) / link re-established
    int real_freq_khz;
} sdio_link_host_stats_t;

// Reset the C6, bring up the SDIO card and start the receive task. A C6 that
// does not answer within boot_timeout_ms leaves the link down (not an error);
// the receive task keeps trying. `cb` runs in
// the receive task for every message from the C6; the payload is valid only
// during the call.
esp_err_t sdio_link_host_start(const sdio_link_host_config_t *config, sdio_msg_cb_t cb, void *ctx);

// Pulse the C6's CHIP_PU and redo the bring-up (e.g. to boot a new image).
// The link is down meanwhile: sends fail with ESP_ERR_INVALID_STATE. On
// failure the link stays down and the receive task recovers it as above.
esp_err_t sdio_link_host_reset_c6(uint32_t timeout_ms);

// false while the C6 is rebooting / the link is being re-established. The
// receive task notices a C6 that rebooted by itself within about a second
// (idle health check of the ready register) or on the next transfer error, and
// re-establishes the link. A C6 that stays silent is reset only after 30 s and
// at most 3 times (so it can be reflashed over its UART meanwhile).
bool sdio_link_host_is_up(void);

// Send one message to the C6. Fails with ESP_ERR_INVALID_STATE while the link
// is down. Blocks up to `timeout_ms` for a free slave
// receive buffer. Safe from several tasks.
esp_err_t sdio_link_host_send(uint8_t plane, const void *payload, uint16_t len, uint32_t timeout_ms);

void sdio_link_host_get_stats(sdio_link_host_stats_t *out);

#ifdef __cplusplus
}
#endif
