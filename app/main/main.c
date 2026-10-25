// Standalone winject SDIO proxy on the Waveshare ESP32-P4-WIFI6-DEV-KIT:
// Ethernet (IP101, DHCP) and the winject proxy, which resets the C6 and
// exposes it as a winject radio on UDP 22 (m-plane) and 9000 (d-plane).

#include <inttypes.h>
#include <stdio.h>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "winject_proxy.h"

static const char *TAG = "winject_p4";

#define ETH_PHY_ADDR 1
#define ETH_PHY_RESET_GPIO 51

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    printf("WINJECT_P4_IP ip=" IPSTR "\n", IP2STR(&ev->ip_info.ip));
    fflush(stdout);
}

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Ethernet link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Ethernet link down");
    }
}

static esp_err_t start_ethernet(void)
{
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(netif, "winject-esp32c6");

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RESET_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    esp_err_t err = esp_eth_driver_install(&eth_cfg, &eth);
    if (err != ESP_OK) {
        return err;
    }
    esp_netif_attach(netif, esp_eth_new_netif_glue(eth));
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    return esp_eth_start(eth);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    err = start_ethernet();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet: %s", esp_err_to_name(err));
    }

    winject_proxy_config_t cfg = WINJECT_PROXY_CONFIG_DEFAULT();
    err = winject_proxy_start(&cfg);
    if (err != ESP_OK) {
        printf("WINJECT_P4_FAILED err=0x%x\n", err);
        return;
    }
    printf("WINJECT_P4_READY\n");
    fflush(stdout);

    // Periodic counters on the console.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        winject_proxy_stats_t ps;
        sdio_link_host_stats_t ls;
        winject_proxy_get_stats(&ps);
        sdio_link_host_get_stats(&ls);
        printf("WINJECT_P4_STATS mreq=%" PRIu32 " mrep=%" PRIu32 " mdrop=%" PRIu32
               " ddown=%" PRIu32 " ddown_drop=%" PRIu32 " dup=%" PRIu32 " dup_nopeer=%" PRIu32
               " dup_fail=%" PRIu32 " dup_evict=%" PRIu32 " link_rx_bad=%" PRIu32 " link_rx_err=%" PRIu32
               " link_tx_err=%" PRIu32 " link_downs=%" PRIu32 " link_ups=%" PRIu32 "\n",
               ps.mplane_requests, ps.mplane_replies, ps.mplane_dropped, ps.dplane_down,
               ps.dplane_down_dropped, ps.dplane_up, ps.dplane_up_no_peer, ps.dplane_up_failed,
               ps.dplane_up_evicted, ls.rx_bad, ls.rx_errors, ls.tx_errors, ls.link_downs, ls.link_ups);
        fflush(stdout);
    }
}
