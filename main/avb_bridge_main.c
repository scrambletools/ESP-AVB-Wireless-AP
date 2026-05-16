/*
 * Copyright 2026 Scramble Tools
 * License: MIT
 *
 * ESP-AVB-Bridge — Ethernet ↔ Wi-Fi AVB bridge (access point).
 *
 * Hardware: Waveshare ESP32-P4-WiFi6-PoE-ETH (ESP1). The P4 owns
 * Ethernet + the AVB stack; the onboard ESP32-C6 is the Wi-Fi
 * co-processor reached over SDIO bus (CMD=GPIO18, CLK=GPIO19,
 * D0=GPIO15)
 *
 * Wi-Fi access on the host side is via Espressif's `esp_hosted` /
 * `esp_wifi_remote` managed components: standard esp_wifi_* APIs are
 * RPC'd transparently over SDIO to the coprocessor. The coprocessor
 * runs Espressif's upstream ESP-Hosted firmware unmodified — we
 * extend functionality from the host side rather than fork it.
 */

#include "avbbridge.h"
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
#include <esp_ptp.h>
#include <esp_vfs_l2tap.h>
#include <esp_wifi.h>
#include <ethernet_init.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <string.h>

static const char *TAG = "avb_bridge";
static esp_eth_handle_t s_eth_handle;
static char s_avb_eth_interface[10];
static esp_netif_t *s_wifi_ap_netif = NULL;
static SemaphoreHandle_t s_ap_started = NULL;

/* WIFI_EVENT_AP_START handler — releases the semaphore that gates
 * avb_start so port[1] init reads a populated MAC instead of zeros.
 * The Wi-Fi driver assigns the AP's MAC into the netif synchronously
 * with this event firing, so esp_netif_get_mac is reliable from here on. */
static void on_ap_start(void *arg, esp_event_base_t base, int32_t id,
                        void *data) {
  (void)arg;
  (void)base;
  (void)id;
  (void)data;
  if (s_ap_started) {
    xSemaphoreGive(s_ap_started);
  }
}

/* SoftAP STA-association count, kept in sync with esp_avb via
 * avb_bridge_set_wifi_ap_sta_count(). esp_avb's MRP LeaveAll
 * suppression reads it back to skip the periodic burst while no
 * client is listening. esp_event handlers run serialised so the
 * counter doesn't need atomicity beyond what `volatile` provides. */
static volatile unsigned int s_ap_sta_count = 0;
static void on_ap_sta_connected(void *arg, esp_event_base_t base, int32_t id,
                                void *data) {
  (void)arg; (void)base; (void)id; (void)data;
  s_ap_sta_count++;
  avb_bridge_set_wifi_ap_sta_count(s_ap_sta_count);
  ESP_LOGI(TAG, "AP STA associated; count=%u", s_ap_sta_count);
}
static void on_ap_sta_disconnected(void *arg, esp_event_base_t base,
                                   int32_t id, void *data) {
  (void)arg; (void)base; (void)id; (void)data;
  if (s_ap_sta_count > 0) s_ap_sta_count--;
  avb_bridge_set_wifi_ap_sta_count(s_ap_sta_count);
  ESP_LOGI(TAG, "AP STA disassociated; count=%u", s_ap_sta_count);
}

/* SoftAP defaults. Open auth for initial proof-of-concept;
 * encrypted Wi-Fi (WPA2/3) is in the plan. */
#define AVB_AP_SSID "ESP-AVB-Bridge"
#define AVB_AP_CHANNEL 6
#define AVB_AP_MAX_CONN 4
/* Beacon interval — TUs of 1024 µs. 100 TU = 102.4 ms, the spec
 * default. IEEE 802.1AS-2020 §12.8.2 prefers ~125 ms
 * (logSyncInterval = -3) for gPTP over Wi-Fi; ESP-IDF documents
 * beacon_interval in multiples of 100, so we accept the small
 * mismatch. May revisit once beacon-IE jitter is measured. */
#define AVB_AP_BEACON_INTERVAL 100

static void init_ethernet_and_netif(void) {
  /* The default event loop may already exist by the time we get here
   * (esp_ptp's ptp_beacon_ie.c creates it from a constructor so it
   * can register a WIFI_EVENT_AP_START handler). ESP_ERR_INVALID_STATE
   * means "already created", which is fine. */
  esp_err_t loop_r = esp_event_loop_create_default();
  if (loop_r != ESP_OK && loop_r != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_r);
  }

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
  ESP_ERROR_CHECK(esp_eth_driver_install(&config, &s_eth_handle));
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_vfs_l2tap_intf_register(NULL));

  esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
  esp_netif_config_t cfg = {.base = &base,
                            .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
  base.if_key = "ETH_0";
  base.if_desc = "eth0";
  base.route_prio = 50;
  esp_netif_t *eth_netif = esp_netif_new(&cfg);

  ESP_ERROR_CHECK(
      esp_netif_attach(eth_netif, esp_eth_new_netif_glue(s_eth_handle)));

  memcpy(s_avb_eth_interface, base.if_key, strlen(base.if_key));
  ESP_LOGI(TAG, "AVB Ethernet interface: %s", s_avb_eth_interface);

  ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));
}

/* Bring up the SoftAP via esp_wifi_remote (transparent SDIO RPC to
 * the onboard coprocessor). The first esp_wifi_init call drives the
 * SDIO host-coprocessor handshake; if the coprocessor is unreachable
 * (wedged / firmware mismatch / cable issue) any of these esp_wifi_*
 * RPCs can return failure. We DON'T ESP_ERROR_CHECK them — an abort
 * here would put the P4 into a reboot loop. Instead we propagate the
 * error to app_main, which drops into wired-only degraded mode. */
#define WIFI_CHECK(call, what)                                                 \
  do {                                                                         \
    esp_err_t _e = (call);                                                     \
    if (_e != ESP_OK) {                                                        \
      ESP_LOGE(TAG, "%s failed (%s) — coprocessor unreachable; "               \
                    "bridge will run wired-only",                              \
               (what), esp_err_to_name(_e));                                   \
      return _e;                                                               \
    }                                                                          \
  } while (0)
static esp_err_t init_wifi_softap(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  /* Create the AP netif. ESP_NETIF_FLAG_AUTOUP keeps the netif up
   * regardless of L3 (we have no IP on the AP — it's an L2 bridge
   * member). The bridge example uses the same flag pattern.
   *
   * if_key="WIFI_0" matches what avb_config.wifi_interface passes
   * down so esp_avb's port[1] L2TAP fds bind to this exact netif. */
  esp_netif_inherent_config_t ap_inherent =
      ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
  ap_inherent.flags = ESP_NETIF_FLAG_AUTOUP;
  ap_inherent.ip_info = NULL;
  ap_inherent.if_key = "WIFI_0";
  ap_inherent.if_desc = "wifi0";
  s_wifi_ap_netif = esp_netif_create_wifi(WIFI_IF_AP, &ap_inherent);
  WIFI_CHECK(esp_wifi_set_default_wifi_ap_handlers(),
             "esp_wifi_set_default_wifi_ap_handlers");

  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  WIFI_CHECK(esp_wifi_init(&wcfg), "esp_wifi_init");
  WIFI_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM), "esp_wifi_set_storage");

  wifi_config_t wifi_cfg = {
      .ap =
          {
              .ssid = AVB_AP_SSID,
              .ssid_len = strlen(AVB_AP_SSID),
              .channel = AVB_AP_CHANNEL,
              .password = "",
              .max_connection = AVB_AP_MAX_CONN,
              .authmode = WIFI_AUTH_OPEN,
              .beacon_interval = AVB_AP_BEACON_INTERVAL,
              /* FTM responder so wireless endpoints can measure
               * peer-delay against us per Path C. The coprocessor HW
               * supports it; ESP-Hosted RPCs the flag transparently. */
              .ftm_responder = true,
          },
  };

  WIFI_CHECK(esp_wifi_set_mode(WIFI_MODE_AP), "esp_wifi_set_mode");
  WIFI_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg), "esp_wifi_set_config");
  /* Power save off on the AP — required for predictable beacon
   * timing once we start publishing FollowUpInformation in the
   * beacon Vendor IE. Leaving it off from boot avoids a
   * later runtime toggle. */
  WIFI_CHECK(esp_wifi_set_ps(WIFI_PS_NONE), "esp_wifi_set_ps");

  /* Set up the AP_START gate before esp_wifi_start so we don't miss
   * the event. esp_avb's port[1] init reads the netif MAC immediately;
   * if we don't wait, it gets zeros (the driver populates the MAC
   * synchronously with WIFI_EVENT_AP_START, not with esp_wifi_start). */
  s_ap_started = xSemaphoreCreateBinary();
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                             on_ap_start, NULL));
  /* Long-lived STA-count handlers — feed esp_avb's LeaveAll suppression. */
  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_sta_connected, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_sta_disconnected, NULL));
  WIFI_CHECK(esp_wifi_start(), "esp_wifi_start");
  if (xSemaphoreTake(s_ap_started, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGW(TAG, "Wi-Fi AP_START event did not fire within 5s; "
                  "port[1] MAC may be zero");
  }
  esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START, on_ap_start);

  ESP_LOGI(TAG, "Wi-Fi SoftAP up: SSID='%s' ch=%d beacon=%d TU max_conn=%d",
           AVB_AP_SSID, AVB_AP_CHANNEL, AVB_AP_BEACON_INTERVAL,
           AVB_AP_MAX_CONN);
  return ESP_OK;
}
#undef WIFI_CHECK

void app_main(void) {
  struct timespec cur_time;

  init_ethernet_and_netif();
  ESP_LOGI(TAG, "Ethernet started");

  /* Start ptpd on the wired side first (port 0 = eth_hwts). This
   * spawns the daemon task and opens the L2TAP socket; once
   * CLOCK_PTP_SYSTEM is readable below, ptp_initialize_state has
   * finished and the daemon is in its main loop. */
  ptpd_start_port(0, s_avb_eth_interface, ptp_port_medium_eth_hwts);

  while (clock_gettime(CLOCK_PTP_SYSTEM, &cur_time) == -1) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  esp_err_t wifi_rc = init_wifi_softap();
  if (wifi_rc != ESP_OK) {
    /* Coprocessor / SDIO failed. Wired ptpd (started above on port 0)
     * keeps running in its own task; we just park app_main here so the
     * P4 stays alive instead of cascading into a reboot. Recovery
     * requires a real C6 reset — see AGENTS.md for the manual
     * procedure. */
    ESP_LOGW(TAG, "Bridge entering wired-only degraded mode "
                  "(Wi-Fi init returned %s). Wired ptpd continues on "
                  "port 0; AVB stack is NOT started.",
             esp_err_to_name(wifi_rc));
    while (1) {
      vTaskDelay(pdMS_TO_TICKS(30000));
      ESP_LOGW(TAG, "heartbeat (degraded, wired ETH + ptpd only)");
    }
  }

  /* Attach the Wi-Fi port to the running daemon (port 1 = wifi_ftm,
   * SoftAP). No socket opens; Sync transport is the SoftAP's 802.11
   * Beacon Vendor IE driven by the sync_egress_cb registered by
   * esp_ptp/ptp_beacon_ie.c on WIFI_EVENT_AP_START. The daemon's
   * periodic-send loop fires the callback at the configured gPTP
   * Sync interval; peer-delay on this port is FTM-driven (none on
   * the AP side — STAs initiate FTM toward us). */
  ptpd_start_port(1, "WIFI_0", ptp_port_medium_wifi_ftm);

  /* AVB stack — bridge role. NUM_PORTS=2: port[0]=Ethernet (EMAC),
   * port[1]=Wi-Fi AP (over esp_wifi_remote → onboard C6). The L2
   * forwarder runs out of esp_avb's avb_unified_rx_cb on both
   * ingress hooks (EMAC for port 0, WIFI_IF_AP for port 1). */
  avb_config_s avb_config = AVB_DEFAULT_CONFIG();
  avb_config.entity_name = "AVB Bridge";
  avb_config.eth_handle = s_eth_handle;
  avb_config.eth_interface = "ETH_0";
  avb_config.wifi_interface = "WIFI_0";

  bool enable = true;
  if (esp_eth_ioctl(s_eth_handle, ETH_CMD_S_PROMISCUOUS, &enable) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set ethernet to promiscuous mode");
    abort();
  }
  /* IDF's ETH_CMD_S_PROMISCUOUS only sets gmacff.pmode (unicast). The
   * GMAC has a separate frame-filter bit gmacff.pam ("pass all
   * multicast") for multicast destinations. Without this, only MACs
   * that lwIP has subscribed via eth_set_mac_filter pass through —
   * which on the bridge means we'd silently drop AVTP (91:e0:f0:01:..)
   * and MVRP (01:80:c2:00:00:21) and the bridge classifier would
   * never see them. */
  if (esp_eth_ioctl(s_eth_handle, ETH_CMD_S_ALL_MULTICAST, &enable) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable pass-all-multicast on EMAC");
    abort();
  }

  avb_start(&avb_config);

  /* Beacon-IE publish is fully owned by esp_ptp. The daemon's
   * periodic-send loop fires the sync_egress_cb on the wifi_ftm port
   * at the gPTP Sync interval; esp_ptp/ptp_beacon_ie.c registers the
   * callback on WIFI_EVENT_AP_START and packs the daemon-marshalled
   * FollowUpInformation bytes into a Vendor IE for the coprocessor
   * to set via esp_wifi_set_vendor_ie(). Nothing for this host to do
   * past bringing up the SoftAP and attaching port 1 above. */

  ESP_LOGI(TAG, "AVB bridge up — Ethernet + Wi-Fi AP, L2 forwarder armed");

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    uint32_t eth_ok = 0, eth_fail = 0, wifi_ok = 0, wifi_fail = 0, wifi_oom = 0;
    avb_bridge_forward_stats(&eth_ok, &eth_fail, &wifi_ok, &wifi_fail,
                             &wifi_oom);
    ESP_LOGI(TAG,
             "heartbeat  fwd eth=%lu/%lu  wifi=%lu/%lu  oom=%lu  STA=%u",
             (unsigned long)eth_ok, (unsigned long)eth_fail,
             (unsigned long)wifi_ok, (unsigned long)wifi_fail,
             (unsigned long)wifi_oom, avb_bridge_wifi_ap_sta_count());
  }
}
