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
//   Queue d-plane:      instead of UDP 9000, when dplane_tx_queue,
//                       dplane_rx_queue and dplane_reactor are all set: the
//                       same datagrams over in-process bfc queues, for
//                       winject-l3 on the same P4 (its radio.dplane_tx /
//                       radio.dplane_rx / radio.reactor). Every C6 message
//                       goes to dplane_rx_queue; no registration is needed.
//   HTTP (http_port):   GET / status; POST /update/c6 with the raw C6 image
//                       (curl --data-binary @winject-esp32c6.bin) streams it
//                       to the C6 over the OTA plane, resets the C6 and checks
//                       the new image answers. POST /update/p4 updates the P4
//                       itself (needs a two-slot partition table); the new
//                       image is kept once it is reached over the network.

#include <stddef.h>
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
    // Queue d-plane, NULL = UDP dplane_port. The host registers both queues
    // (bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>)
    // and the bfc::cv_reactor<> (bfc::cv_reactor_registry), and runs the
    // reactor, before winject_proxy_start(); the proxy holds them from then on.
    const char *dplane_tx_queue;  // consumed: inject MPDUs, registrations (manager TX)
    const char *dplane_rx_queue;  // produced: C6 forward frames (manager RX)
    const char *dplane_reactor;
} winject_proxy_config_t;

#define WINJECT_PROXY_CONFIG_DEFAULT()                                              \
    {                                                                               \
        .link = SDIO_LINK_HOST_CONFIG_DEFAULT(), .mplane_port = 22,                 \
        .dplane_port = 9000, .mplane_task_prio = 10, .dplane_task_prio = 15,        \
        .dplane_send_timeout_ms = 20, .http_port = 80, .dplane_tx_queue = NULL,     \
        .dplane_rx_queue = NULL, .dplane_reactor = NULL,                            \
    }

typedef struct {
    uint32_t mplane_requests, mplane_replies, mplane_dropped;
    uint32_t dplane_down;          // UDP -> C6 messages sent
    uint32_t dplane_down_dropped;  // no C6 buffer in time, or oversized queue datagram
    uint32_t dplane_up;            // C6 -> UDP datagrams sent / queue frames pushed
    uint32_t dplane_up_no_peer;    // no registration yet (UDP)
    uint32_t dplane_up_failed;     // sendto failed (UDP)
    uint32_t dplane_up_evicted;    // full rx queue dropped its oldest frame (queue; also in dplane_up)
} winject_proxy_stats_t;

// Resets the C6, brings up the link and starts both planes. Call after
// esp_netif_init(); the sockets bind to any address. Fails with
// ESP_ERR_NOT_FOUND when the queue d-plane cannot be bound (a name missing or
// not registered, both names on one queue, a queue of capacity 0).
esp_err_t winject_proxy_start(const winject_proxy_config_t *config);

void winject_proxy_get_stats(winject_proxy_stats_t *out);

#ifdef __cplusplus
}
#endif
