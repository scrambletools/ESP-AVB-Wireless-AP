/*
 * Copyright 2026 Scramble Tools
 * License: MIT
 *
 * ESP32-C6 co-processor firmware for ESP-AVB-Wireless-AP.
 *
 * Phase 4 skeleton — boots, prints identity, idles. The real
 * responsibilities land in Phase 5+:
 *   - SDIO slave: 6-channel framing with the P4 host
 *       channel 0 = bridged 802.3 data frames
 *       channel 1 = control / association events
 *       channel 2 = FTM peer-delay reports (per-STA)
 *       channel 3 = AP beacon Vendor IE updates (FollowUpInformation)
 *       channel 4 = unicast 802.1AS frame inject (Announce, Signaling)
 *       channel 5 = log / debug
 *   - Wi-Fi SoftAP at 125 ms beacon interval (matches gPTP §12.8.2
 *     currentLogSyncInterval = -3)
 *   - FTM responder per associated STA
 *   - Optional FTM initiator round to characterize peer-delay
 */

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "avb_ap_c6";

void app_main(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  ESP_LOGI(TAG, "C6 co-processor up. WIFI_STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  ESP_LOGI(TAG, "Phase 4 skeleton — SDIO slave + AP not yet wired");

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "heartbeat");
  }
}
