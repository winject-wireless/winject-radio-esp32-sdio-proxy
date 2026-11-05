#pragma once

// udp_ota: firmware update and config-file access over UDP, for this P4's app
// and for the C6 radio behind the SDIO link. Wire: core/wire.hpp; client:
// tools/udp_ota.py. winject_proxy_start() starts it (winject_proxy_config_t
// ota_*); one request per datagram, every reply to its sender, so it is
// reachable over Ethernet and, through an L3 relay upstream, over the air.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t port;
    // Directory for ota_fs_* (e.g. a mounted SPIFFS config partition); NULL =
    // no file access.
    const char *fs_dir;
    // An OTA, or an open file, whose client sends nothing for this long is
    // aborted / closed.
    uint32_t idle_timeout_ms;
    // true while an image comes in or the C6 is written, false after (e.g. to
    // pause video). Runs on udp_ota's tasks.
    void (*on_active)(bool active, void *ctx);
    void *ctx;
    int task_prio;
    int task_core;  // -1 = unpinned
    uint32_t task_stack;
} udp_ota_config_t;

#define UDP_OTA_CONFIG_DEFAULT()                                                                 \
    {                                                                                            \
        .port = 3232, .fs_dir = NULL, .idle_timeout_ms = 30000, .on_active = NULL, .ctx = NULL, \
        .task_prio = 5, .task_core = -1, .task_stack = 6 * 1024,                                 \
    }

// Starts the server task. Call once, after esp_netif_init() and before the
// SDIO link delivers OTA-plane replies (udp_ota_on_c6_reply).
esp_err_t udp_ota_start(const udp_ota_config_t *config);

// The SDIO link receive task: an OTA-plane reply from the C6.
void udp_ota_on_c6_reply(const uint8_t *payload, uint16_t len);

// Marks a newly updated P4 image valid (cancels rollback). Called on the
// first request reaching the P4 over the network (proxy m-plane / d-plane,
// udp_ota): the image is kept once it is reachable.
void udp_ota_confirm_p4_image(void);

#ifdef __cplusplus
}
#endif
