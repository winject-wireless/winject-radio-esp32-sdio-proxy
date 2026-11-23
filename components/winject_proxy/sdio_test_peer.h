#pragma once

// P4 side of the SDIO bandwidth test (winject-radio-esp32c6's sdio_test.h and
// docs/sdio-link.md "Test plane"). Passive: the C6 runs the test; this sends
// test packets on bw_tx, counts the C6's, and answers get_rx_stats with its
// counts, CPU and memory-probe figures.

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the peer task (sends bw_tx packets and replies). Call before the link
// delivers messages.
esp_err_t sdio_test_peer_start(int task_prio);

// The SDIO link receive task: one test-plane message from the C6.
void sdio_test_peer_on_message(const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif
