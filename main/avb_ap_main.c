/*
 * Copyright 2026 Scramble Tools
 * License: MIT
 *
 * ESP-AVB-Wireless-AP — Ethernet ↔ Wi-Fi AVB bridge (Phase 4 skeleton).
 *
 * Hardware target: Waveshare ESP32-P4-WiFi6-PoE-ETH combo board.
 * The P4 owns Ethernet + the AVB stack; the onboard ESP32-C6 (driven
 * by c6_firmware/) is the Wi-Fi co-processor reached over SDIO.
 *
 * Phase 4 lands only the project skeleton + Ethernet bring-up + ptpd.
 * The SDIO transport, beacon-IE publish path, and bridge-role logic
 * (FQTSS, MSRP MAP, 75% admission, Class A drop on Wi-Fi egress)
 * land in Phase 5+.
 */

#include "esp_avb.h"
#include "esp_eth_clock.h"
#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_eth.h>
#include <esp_eth_phy_ip101.h>
#include <esp_event.h>
#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_vfs_l2tap.h>
#include <ethernet_init.h>
#include <ptpd.h>
#include <sdkconfig.h>
#include <string.h>

static const char *TAG = "avb_ap";
esp_eth_handle_t eth_handle;
char avb_interface[10];

/* Initialize Ethernet and netif on the P4 side. Same pattern as the
 * wired example; the bridge will add a Wi-Fi netif (over SDIO) in
 * Phase 5. */
static void init_ethernet_and_netif(void) {
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

  emac_config.dma_burst_len = ETH_DMA_BURST_LEN_32;
  emac_config.intr_priority = 0;
  mac_config.rx_task_stack_size = 16384;
  mac_config.rx_task_prio = 22;
  phy_config.phy_addr = 1;
  phy_config.reset_gpio_num = 5;

  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);

  esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
  ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_vfs_l2tap_intf_register(NULL));

  esp_netif_inherent_config_t esp_netif_base_config =
      ESP_NETIF_INHERENT_DEFAULT_ETH();
  esp_netif_config_t esp_netif_config = {
      .base = &esp_netif_base_config, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
  esp_netif_base_config.if_key = "ETH_0";
  esp_netif_base_config.if_desc = "eth0";
  esp_netif_base_config.route_prio = 50;
  esp_netif_t *eth_netif = esp_netif_new(&esp_netif_config);

  ESP_ERROR_CHECK(
      esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

  memcpy(avb_interface, esp_netif_base_config.if_key,
         strlen(esp_netif_base_config.if_key));
  ESP_LOGI(TAG, "AVB interface: %s", avb_interface);

  ESP_ERROR_CHECK(esp_eth_start(eth_handle));
}

void app_main(void) {
  struct timespec cur_time;

  init_ethernet_and_netif();
  ESP_LOGI(TAG, "Ethernet started");

  ptpd_start(avb_interface);

  while (clock_gettime(CLOCK_PTP_SYSTEM, &cur_time) == -1) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  /* Phase 4 skeleton: AVB stack starts in bridge role but the bridge
   * forwarding path is not yet implemented. Behaves as an idle endpoint
   * pending Phase 5. */
  avb_config_s avb_config = AVB_DEFAULT_CONFIG();
  avb_config.entity_name = "AVB Bridge";
  avb_config.eth_handle = eth_handle;
  avb_config.eth_interface = "ETH_0";

  esp_eth_io_cmd_t cmd = ETH_CMD_S_PROMISCUOUS;
  bool promiscuous = true;
  esp_err_t err = esp_eth_ioctl(eth_handle, cmd, &promiscuous);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set ethernet to promiscuous mode");
    abort();
  }

  avb_start(&avb_config);

  ESP_LOGI(TAG, "AVB bridge skeleton up; SDIO transport not yet wired");

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "heartbeat (bridge skeleton)");
  }
}
