/*
 * Copyright 2026 Scramble Tools
 * License: MIT
 *
 * Bridge perf-test mode (CONFIG_AVB_BRIDGE_PERF_TEST_MODE).
 *
 * Replaces the production bring-up entirely. Brings up only
 * NVS + esp_event + WiFi SoftAP, then spawns one TX task that
 * loops esp_wifi_internal_tx with a pre-built AVTP-AAF frame
 * to measure the SDIO -> coprocessor -> 802.11 path on its own.
 *
 * No PTP, no AVB, no Ethernet — the production code is gated out
 * in avb_bridge_main.c. Counters are plain volatile, written only
 * by the TX task and read only by the 1 Hz stats task, so the hot
 * path takes no logging or synchronisation cost.
 */

#include "avb_bridge_perf_test.h"
#include "sdkconfig.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "perf_tx";

/* esp_wifi internal TX: same forward-decl pattern as esp_avb/avbnet.c so
 * this file doesn't gain a hard esp_private include. esp_wifi (linked
 * through esp_wifi_remote on the P4 host) provides the symbol. */
extern esp_err_t esp_wifi_internal_tx(int wifi_if, void *buffer, size_t len);

#define PERF_AP_SSID    "ESP-AVB-Bridge"
#define PERF_AP_CHANNEL 6
#define PERF_AP_MAX_CONN 4
#define PERF_AP_BEACON_INTERVAL 100

#define ETH_HDR_LEN  14
#define VLAN_TAG_LEN  4
#define AVTP_HDR_LEN 24
#define PERF_MAX_PAYLOAD 1400

/* Per-frame physical-layer overhead for wired Ethernet accounting:
 * preamble + SFD (8) + CRC (4) + IFG (12) = 24 B. 802.11 actual
 * airtime is higher (MAC header, LLC/SNAP, ACK, PLCP) but isn't
 * measurable from firmware; this lets the user compare against a
 * link-rate ceiling on the same "total wire bytes" basis the WiFi
 * radio reports throughput in. */
#define PHY_FRAMING_BYTES 24

/* Hot-path counters: one writer (perf_tx_task), one reader
 * (perf_stats_task at 1 Hz). 64-bit values are read in two halves on
 * 32-bit RISC-V — acceptable here because the stats are reported as
 * deltas over a 1 s window and a torn read just shifts a few packets
 * across the window boundary. */
static volatile uint64_t s_tx_pkts = 0;
static volatile uint64_t s_tx_bytes_l2 = 0;
static volatile uint64_t s_tx_bytes_wire = 0;  /* L2 + PHY_FRAMING_BYTES per frame */
static volatile uint64_t s_tx_fail = 0;
static volatile uint64_t s_tx_busy = 0;  /* wifi TX queue full -> retry */
static volatile uint32_t s_sta_count = 0;
static volatile bool s_tx_done = false;
static volatile int64_t s_tx_start_us = 0;
static volatile int64_t s_tx_end_us = 0;

static int parse_mac(const char *s, uint8_t out[6]) {
  unsigned a, b, c, d, e, f;
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f) != 6) {
    return -1;
  }
  out[0] = a; out[1] = b; out[2] = c;
  out[3] = d; out[4] = e; out[5] = f;
  return 0;
}

static int parse_stream_id(const char *s, uint8_t out[8]) {
  unsigned v[8];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x:%x:%x",
             &v[0], &v[1], &v[2], &v[3],
             &v[4], &v[5], &v[6], &v[7]) == 8) {
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)v[i];
    return 0;
  }
  unsigned long long u = 0;
  if (sscanf(s, "%llx", &u) == 1) {
    for (int i = 0; i < 8; i++) {
      out[i] = (uint8_t)((u >> (56 - 8 * i)) & 0xff);
    }
    return 0;
  }
  return -1;
}

static void build_frame(uint8_t *frame, int *frame_len,
                        const uint8_t src_mac[6]) {
  uint8_t dest_mac[6];
  uint8_t stream_id[8];
  if (parse_mac(CONFIG_AVB_BRIDGE_PERF_TEST_DEST_MAC, dest_mac) != 0) {
    ESP_LOGW(TAG, "bad dest MAC '%s', using 91:e0:f0:00:fe:00",
             CONFIG_AVB_BRIDGE_PERF_TEST_DEST_MAC);
    dest_mac[0] = 0x91; dest_mac[1] = 0xe0; dest_mac[2] = 0xf0;
    dest_mac[3] = 0x00; dest_mac[4] = 0xfe; dest_mac[5] = 0x00;
  }
  if (parse_stream_id(CONFIG_AVB_BRIDGE_PERF_TEST_STREAM_ID, stream_id) != 0) {
    ESP_LOGW(TAG, "bad stream id '%s', using default",
             CONFIG_AVB_BRIDGE_PERF_TEST_STREAM_ID);
    memset(stream_id, 0, 8);
    stream_id[7] = 1;
  }

  int payload_bytes = CONFIG_AVB_BRIDGE_PERF_TEST_PAYLOAD_BYTES;
  if (payload_bytes < 0) payload_bytes = 0;
  if (payload_bytes > PERF_MAX_PAYLOAD) payload_bytes = PERF_MAX_PAYLOAD;

  int total = ETH_HDR_LEN + VLAN_TAG_LEN + AVTP_HDR_LEN + payload_bytes;
  *frame_len = total;
  memset(frame, 0, total);

  /* Ethernet header */
  memcpy(frame + 0, dest_mac, 6);
  memcpy(frame + 6, src_mac, 6);
  frame[12] = 0x81;
  frame[13] = 0x00;

  /* VLAN tag: PCP=3 (Class A), VID=2 (Milan AAF default) */
  uint16_t tci = (3u << 13) | (2u & 0x0fffu);
  frame[14] = (uint8_t)(tci >> 8);
  frame[15] = (uint8_t)(tci & 0xff);
  frame[16] = 0x22;
  frame[17] = 0xf0; /* inner AVTP ethertype */

  /* AVTP common header (24 B). Subtype AAF, sv=1, tv=1. Default frame
   * shape is Milan AAF: 48 kHz, 8 ch, 24-bit-in-32 container.
   * Sequence number at byte 18+2 is bumped per-packet by the TX loop. */
  uint8_t *avtp = frame + ETH_HDR_LEN + VLAN_TAG_LEN;
  avtp[0]  = 0x02; /* subtype: AAF */
  avtp[1]  = 0x81; /* sv=1, version=0, mr=0, tv=1 */
  avtp[2]  = 0;    /* sequence number (per-packet) */
  avtp[3]  = 0x00; /* tu=0 */
  memcpy(avtp + 4, stream_id, 8);
  /* avtp[12..15] presentation_timestamp: zero for perf test */
  avtp[16] = 0x04; /* AAF format: 24-bit int (carried in 32-bit container) */
  avtp[17] = (4u << 4); /* nsr = 4 (48 kHz) in upper nibble */
  avtp[18] = 8;    /* channels_per_frame: Milan 8 ch default */
  avtp[19] = 24;   /* bit_depth */
  avtp[20] = (uint8_t)((payload_bytes >> 8) & 0xff);
  avtp[21] = (uint8_t)(payload_bytes & 0xff);
  avtp[22] = 0x00;
  avtp[23] = 0x00;
  /* payload bytes left zeroed */
}

static void perf_tx_task(void *arg) {
  (void)arg;

  static uint8_t frame[ETH_HDR_LEN + VLAN_TAG_LEN + AVTP_HDR_LEN +
                       PERF_MAX_PAYLOAD];
  uint8_t src_mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_AP, src_mac);
  ESP_LOGI(TAG, "AP MAC %02x:%02x:%02x:%02x:%02x:%02x",
           src_mac[0], src_mac[1], src_mac[2],
           src_mac[3], src_mac[4], src_mac[5]);

  int frame_len = 0;
  build_frame(frame, &frame_len, src_mac);
  uint32_t interval_us = CONFIG_AVB_BRIDGE_PERF_TEST_INTERVAL_US;
  ESP_LOGI(TAG, "frame=%d B payload=%d B interval=%lu us",
           frame_len, (int)CONFIG_AVB_BRIDGE_PERF_TEST_PAYLOAD_BYTES,
           (unsigned long)interval_us);

  /* Hold off TX until at least one STA has associated. Sending into an
   * empty SoftAP just burns CPU and inflates the busy counter with no
   * signal about the end-to-end path. */
  ESP_LOGI(TAG, "waiting for STA association before starting TX");
  while (s_sta_count == 0) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  ESP_LOGI(TAG, "STA associated — starting TX");

  uint32_t duration_sec = CONFIG_AVB_BRIDGE_PERF_TEST_DURATION_SEC;
  s_tx_start_us = esp_timer_get_time();
  int64_t deadline_us = (duration_sec > 0)
      ? s_tx_start_us + (int64_t)duration_sec * 1000000LL
      : 0;

  uint8_t seq = 0;
  while (deadline_us == 0 || esp_timer_get_time() < deadline_us) {
    frame[ETH_HDR_LEN + VLAN_TAG_LEN + 2] = seq++;
    esp_err_t r = esp_wifi_internal_tx(WIFI_IF_AP, frame, frame_len);
    if (r == ESP_OK) {
      s_tx_pkts++;
      s_tx_bytes_l2 += (uint64_t)frame_len;
      s_tx_bytes_wire += (uint64_t)(frame_len + PHY_FRAMING_BYTES);
    } else if (r == ESP_ERR_NO_MEM) {
      /* Wi-Fi TX queue full — yield to let the wifi task drain. The
       * sustained pps we see here is the rate the SDIO + 802.11 path
       * can actually carry; the busy counter just reflects backpressure. */
      s_tx_busy++;
      vTaskDelay(1);
    } else {
      s_tx_fail++;
      vTaskDelay(1);
    }
    if (interval_us > 0) {
      esp_rom_delay_us(interval_us);
    }
  }
  s_tx_end_us = esp_timer_get_time();
  s_tx_done = true;
  ESP_LOGW(TAG, "test duration %lu s reached — TX stopped at pkt %llu",
           (unsigned long)duration_sec, (unsigned long long)s_tx_pkts);
  vTaskDelete(NULL);
}

static void perf_stats_task(void *arg) {
  (void)arg;
  uint64_t last_pkts = 0;
  uint64_t last_bytes_l2 = 0;
  uint64_t last_bytes_wire = 0;
  int64_t last_t = esp_timer_get_time();
  bool summarised = false;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint64_t pkts = s_tx_pkts;
    uint64_t bytes_l2 = s_tx_bytes_l2;
    uint64_t bytes_wire = s_tx_bytes_wire;
    uint64_t fail = s_tx_fail;
    uint64_t busy = s_tx_busy;
    int64_t now = esp_timer_get_time();
    int64_t dt_us = now - last_t;
    if (dt_us <= 0) continue;
    uint64_t dp = pkts - last_pkts;
    uint64_t db_l2 = bytes_l2 - last_bytes_l2;
    uint64_t db_wire = bytes_wire - last_bytes_wire;
    last_pkts = pkts;
    last_bytes_l2 = bytes_l2;
    last_bytes_wire = bytes_wire;
    last_t = now;
    uint64_t pps = (dp * 1000000ULL) / (uint64_t)dt_us;
    double mbps_l2 = (double)(db_l2 * 8ULL) / (double)dt_us;
    double mbps_wire = (double)(db_wire * 8ULL) / (double)dt_us;
    ESP_LOGI(TAG,
             "tx pps=%llu L2=%.2fMbps wire=%.2fMbps fail=%llu busy=%llu total=%llu sta=%lu",
             (unsigned long long)pps, mbps_l2, mbps_wire,
             (unsigned long long)fail, (unsigned long long)busy,
             (unsigned long long)pkts, (unsigned long)s_sta_count);
    if (s_tx_done && !summarised) {
      int64_t run_us = s_tx_end_us - s_tx_start_us;
      if (run_us <= 0) run_us = 1;
      double mean_pps = (double)pkts * 1.0e6 / (double)run_us;
      double mean_l2 = (double)(bytes_l2 * 8ULL) / (double)run_us;
      double mean_wire = (double)(bytes_wire * 8ULL) / (double)run_us;
      ESP_LOGW(TAG,
               "FINAL tx run=%.2fs pkts=%llu L2_bytes=%llu wire_bytes=%llu "
               "fail=%llu busy=%llu mean_pps=%.0f mean_L2=%.2fMbps "
               "mean_wire=%.2fMbps",
               (double)run_us / 1.0e6,
               (unsigned long long)pkts, (unsigned long long)bytes_l2,
               (unsigned long long)bytes_wire,
               (unsigned long long)fail, (unsigned long long)busy,
               mean_pps, mean_l2, mean_wire);
      summarised = true;
    }
  }
}

static void on_ap_event(void *arg, esp_event_base_t base, int32_t id,
                        void *data) {
  (void)arg; (void)base;
  switch (id) {
  case WIFI_EVENT_AP_START:
    ESP_LOGI(TAG, "SoftAP started SSID='%s' ch=%d", PERF_AP_SSID,
             PERF_AP_CHANNEL);
    break;
  case WIFI_EVENT_AP_STACONNECTED: {
    wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
    s_sta_count++;
    ESP_LOGI(TAG, "STA assoc %02x:%02x:%02x:%02x:%02x:%02x count=%lu",
             e->mac[0], e->mac[1], e->mac[2],
             e->mac[3], e->mac[4], e->mac[5],
             (unsigned long)s_sta_count);
    break;
  }
  case WIFI_EVENT_AP_STADISCONNECTED: {
    wifi_event_ap_stadisconnected_t *e =
        (wifi_event_ap_stadisconnected_t *)data;
    if (s_sta_count > 0) s_sta_count--;
    ESP_LOGI(TAG, "STA disassoc %02x:%02x:%02x:%02x:%02x:%02x count=%lu",
             e->mac[0], e->mac[1], e->mac[2],
             e->mac[3], e->mac[4], e->mac[5],
             (unsigned long)s_sta_count);
    break;
  }
  default:
    break;
  }
}

void avb_bridge_perf_test_run(void) {
  ESP_LOGW(TAG, "perf test mode — PTP and AVB NOT started");

  esp_err_t r = nvs_flash_init();
  if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  esp_err_t loop_r = esp_event_loop_create_default();
  if (loop_r != ESP_OK && loop_r != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_r);
  }
  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             on_ap_event, NULL));

  wifi_config_t wifi_cfg = {
      .ap = {
          .ssid = PERF_AP_SSID,
          .ssid_len = strlen(PERF_AP_SSID),
          .channel = PERF_AP_CHANNEL,
          .password = "",
          .max_connection = PERF_AP_MAX_CONN,
          .authmode = WIFI_AUTH_OPEN,
          .beacon_interval = PERF_AP_BEACON_INTERVAL,
      },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());

  /* TX task pinned to core 1 — matches the production sdkconfig
   * (CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1=y) so the WiFi task and the
   * caller end up co-resident, avoiding cross-core hand-offs in the
   * hot path. Stats task left unpinned. */
  xTaskCreatePinnedToCore(perf_tx_task, "perf_tx", 4096, NULL, 10, NULL, 1);
  xTaskCreate(perf_stats_task, "perf_stats", 4096, NULL, 5, NULL);
}
