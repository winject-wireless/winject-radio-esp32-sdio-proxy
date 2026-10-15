#pragma once

// winject_proxy's HTTP server: status page, C6 firmware update over the SDIO
// OTA plane, and the P4's own firmware update.

#include <stdint.h>

#include "esp_err.h"

esp_err_t ota_client_start_http(uint16_t port);

// Mark a newly updated P4 image valid (cancels rollback). Called on the first
// UDP datagram or HTTP request: the image is kept once it is reachable.
void ota_client_confirm_p4_image(void);

// Link receive task: an OTA-plane reply from the C6.
void ota_client_on_reply(const uint8_t *payload, uint16_t len);
