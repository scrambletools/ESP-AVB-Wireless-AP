/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 Scramble Tools
 *
 * SDIO transport between the ESP32-P4 host and the onboard ESP32-C6
 * Wi-Fi coprocessor on ESP-AVB-Bridge. ESP-Hosted-style framing
 * with 6 channels, 12-byte header, credit-based flow control on
 * channel 1 (control). Implementation is Phase 4 in-progress; this
 * header defines the protocol so both sides can agree.
 */

#ifndef SDIO_AVB_LINK_H
#define SDIO_AVB_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channel identifiers — kept in sync between host and slave. */
typedef enum {
  SDIO_AVB_CH_DATA = 0,      /* bridged 802.3 frames, both directions */
  SDIO_AVB_CH_CONTROL = 1,   /* AP up/down, association events, credits */
  SDIO_AVB_CH_FTM_REPORT = 2,/* per-STA peer-delay reports, C6 -> P4 */
  SDIO_AVB_CH_BEACON_IE = 3, /* AP beacon Vendor IE updates, P4 -> C6 */
  SDIO_AVB_CH_AS_INJECT = 4, /* unicast 802.1AS frames, P4 -> C6 */
  SDIO_AVB_CH_DEBUG = 5,     /* logs / debug, both directions */
  SDIO_AVB_CH_COUNT
} sdio_avb_channel_t;

/* 12-byte frame header. Wire-aligned; little-endian fields. */
typedef struct __attribute__((packed)) {
  uint8_t  channel;          /* sdio_avb_channel_t */
  uint8_t  flags;            /* bit 0 = more_fragments, bits 1-7 reserved */
  uint16_t length;           /* payload bytes following the header */
  uint32_t seq;              /* monotonic per-channel; wraps on uint32 */
  uint32_t reserved;         /* future use; sender writes 0 */
} sdio_avb_hdr_t;

#define SDIO_AVB_HDR_SIZE 12u
#define SDIO_AVB_FLAG_MORE_FRAGMENTS (1u << 0)

/* Maximum payload per frame. Sized to fit a full 802.3 MTU plus L2
 * headers without fragmentation. Larger payloads use MORE_FRAGMENTS. */
#define SDIO_AVB_MAX_PAYLOAD 1600u

/* Credit messages on CH_CONTROL announce free buffer count per channel.
 * Format: { uint8_t msg_type=CREDIT_UPDATE, uint8_t channel,
 *           uint16_t free_buffers } */
typedef enum {
  SDIO_AVB_CTRL_CREDIT_UPDATE = 0x01,
  SDIO_AVB_CTRL_AP_UP = 0x10,
  SDIO_AVB_CTRL_AP_DOWN = 0x11,
  SDIO_AVB_CTRL_STA_ASSOC = 0x12,
  SDIO_AVB_CTRL_STA_DISASSOC = 0x13,
} sdio_avb_ctrl_msgtype_t;

/* Receive callback signature. Owns the payload buffer for the duration
 * of the call only; copy if needed beyond that. */
typedef void (*sdio_avb_rx_cb_t)(sdio_avb_channel_t channel,
                                 const void *payload,
                                 size_t length,
                                 void *ctx);

/* Public API — implementation lands in Phase 4 SDIO bring-up. */
int sdio_avb_link_init(void);
int sdio_avb_link_register_rx_cb(sdio_avb_channel_t channel,
                                 sdio_avb_rx_cb_t cb,
                                 void *ctx);
int sdio_avb_link_send(sdio_avb_channel_t channel,
                       const void *payload,
                       size_t length);

#ifdef __cplusplus
}
#endif

#endif /* SDIO_AVB_LINK_H */
