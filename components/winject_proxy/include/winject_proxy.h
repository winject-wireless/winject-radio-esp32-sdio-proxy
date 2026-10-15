#pragma once

// P4 side of winject-radio-esp32c6 (its docs/sdio-link.md, "P4 proxy"): binds the
// Ethernet radio's UDP ports on the P4's lwIP and forwards datagrams unchanged
// over the SDIO link to the C6, so winject-manager sees an ordinary radio.
//
//   UDP 22 (m-plane):   request -> m-plane message; replies -> datagrams to the
//                       source of the most recent request.
//   UDP 9000 (d-plane): every datagram -> d-plane message; 1-23 byte datagrams
//                       (registrations) also make their source the forward
//                       peer; C6 messages -> datagrams to that peer.
//   HTTP (http_port):   GET / status; POST /update/c6 with the raw C6 image
//                       (curl --data-binary @winject-esp32c6.bin) streams it
//                       to the C6 over the OTA plane, resets the C6 and checks
//                       the new image answers. POST /update/p4 updates the P4
//                       itself (needs a two-slot partition table); the new
//                       image is kept once it is reached over the network.

#include <stdint.h>

#include "esp_err.h"
#include "sdio_link_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sdio_link_host_config_t link;
    uint16_t mplane_port;
    uint16_t dplane_port;
    int mplane_task_prio;
    // Below the link's receive task, above the rest: a priority-1 sender gets
    // about one SDIO write per tick under load.
    int dplane_task_prio;
    uint32_t dplane_send_timeout_ms;  // wait for a free C6 buffer, then drop
    uint16_t http_port;               // 0 = no HTTP server
} winject_proxy_config_t;

#define WINJECT_PROXY_CONFIG_DEFAULT()                                              \
    {                                                                               \
        .link = SDIO_LINK_HOST_CONFIG_DEFAULT(), .mplane_port = 22,                 \
        .dplane_port = 9000, .mplane_task_prio = 10, .dplane_task_prio = 15,        \
        .dplane_send_timeout_ms = 20, .http_port = 80,                              \
    }

typedef struct {
    uint32_t mplane_requests, mplane_replies, mplane_dropped;
    uint32_t dplane_down;          // UDP -> C6 messages sent
    uint32_t dplane_down_dropped;  // no C6 buffer in time
    uint32_t dplane_up;            // C6 -> UDP datagrams sent
    uint32_t dplane_up_no_peer;    // no registration yet
    uint32_t dplane_up_failed;     // sendto failed
} winject_proxy_stats_t;

// Resets the C6, brings up the link and starts both UDP planes. Call after
// esp_netif_init(); the sockets bind to any address.
esp_err_t winject_proxy_start(const winject_proxy_config_t *config);

void winject_proxy_get_stats(winject_proxy_stats_t *out);

#ifdef __cplusplus
}
#endif
