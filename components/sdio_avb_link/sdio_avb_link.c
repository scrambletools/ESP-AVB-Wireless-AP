/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 Scramble Tools
 *
 * Host-side SDIO driver for ESP-AVB-Bridge; protocol surface in
 * sdio_avb_link.h.
 */

#include "sdio_avb_link.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "sdio_avb_link";

static sdio_avb_rx_cb_t s_rx_cb[SDIO_AVB_CH_COUNT] = {0};
static void *s_rx_ctx[SDIO_AVB_CH_COUNT] = {0};

int sdio_avb_link_init(void) {
  ESP_LOGW(TAG, "SDIO transport not yet wired");
  return 0;
}

int sdio_avb_link_register_rx_cb(sdio_avb_channel_t channel,
                                 sdio_avb_rx_cb_t cb,
                                 void *ctx) {
  if (channel >= SDIO_AVB_CH_COUNT) {
    return -EINVAL;
  }
  s_rx_cb[channel] = cb;
  s_rx_ctx[channel] = ctx;
  return 0;
}

int sdio_avb_link_send(sdio_avb_channel_t channel,
                       const void *payload,
                       size_t length) {
  (void)channel;
  (void)payload;
  (void)length;
  return -ENOSYS;
}
