/*
 * Copyright 2026 Scramble Tools
 * License: MIT
 *
 * Bridge perf-test mode. When CONFIG_AVB_BRIDGE_PERF_TEST_MODE is set
 * the bridge app_main calls avb_bridge_perf_test_run() instead of the
 * production bring-up — only NVS + esp_event + WiFi SoftAP come up,
 * and a TX task blasts a pre-built AVTP-shaped frame at
 * esp_wifi_internal_tx so the path under measurement is
 * P4 -> SDIO -> C6 -> 802.11 -> STA, with nothing else running.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void avb_bridge_perf_test_run(void);

#ifdef __cplusplus
}
#endif
