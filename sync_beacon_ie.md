
Design: BTC time rides the §12.7 VendorSpecific information element
carrying the entire 802.1AS Follow_Up message (PTP common header +
preciseOriginTimestamp + FollowUpInformation TLV, 76 bytes total).
Peer delay is measured by the STA via IEEE 802.11mc FTM and injected
via `ptpd_inject_peer_delay`. The wire bytes of the §12.7 IE are
spec-compliant.

**Carrier deviation from §12.** IEEE 802.1AS-2020 §12.1.2 specifies
that the §12.7 IE is carried inside TM (Timing Measurement) or FTM
(Fine Timing Measurement) action frames, and that no separate
802.1AS frames are transmitted on 802.11 links ("this clause does
not define any new frames nor the transmission of any frames"). Our
implementation parks the IE in the SoftAP's **Beacon** Vendor IE
instead. The reason: ESP-IDF's public Wi-Fi API
(`esp_wifi_set_vendor_ie()`) only addresses the Beacon/ProbeResp
Vendor IE slots; the FTM responder Vendor IE is not exposed by the
host-side API or by `esp_wifi_remote`. The byte format of the §12.7
IE is exactly the spec; only the carrier frame type changes. The
beacon carrier alone is bounded by beacon TX jitter (≈100 ms) and
the host→coprocessor→TX→air pipeline; with no further mechanism we'd
get only ~50–100 ms phase accuracy. See `espressif.md` in this repo
for the API additions we'd want from IDF to land spec-compliant
§12.7-in-FTM, and the precision improvements they'd unlock.

**Companion Scramble Tools TSF mapping IE.** To recover most of the
precision the spec carrier was designed for, the bridge publishes a
**second** Vendor IE in the same beacon — Scramble-Tools-private
sub-OUI type `0x01`, payload `uint64 µs LE` carrying the bridge's
wifi MAC TSF at coprocessor-publish moment. The host marshals the
template with zeros; the coprocessor's RPC handler reads
`esp_wifi_get_tsf_time(WIFI_IF_AP)` and patches the payload in
immediately before the beacon goes out, so the captured TSF is as
close as possible to the actual beacon TX moment.

The STA parses both IEs and stores `(gPTP_marker_ns,
ap_tsf_marker_us)` as a running pair. When `WIFI_EVENT_FTM_REPORT`
fires (FTM session is initiated separately by the STA against the
bridge's FTM responder), the handler converts the responder's
hardware-timestamped `entries[best].t1` (in bridge TSF pSec) to GM
time via the mapping: `t1_gPTP_ns = gPTP_marker + (t1_us −
ap_tsf_marker) × 1000`. It back-projects the STA local clock to the
FTM RX moment using `entries[best].t2` plus `esp_timer`, and feeds
the pair into the new `ptpd_inject_sync_pair(port, remote_ns,
local_ns)` API. From there the normal PI servo runs (now exercising
the SW clock backend for non-EMAC builds — see Phase 3 below).

This architecture preserves the §12.7 IE byte format unchanged
(byte-format compliant; a standards-aware parser sees it untouched)
and confines the deviation to two things: the carrier (beacon vs
TM/FTM action frame, same as before) and a clearly-labeled
vendor-private mapping IE alongside it. When IDF exposes the FTM
responder Vendor IE slot we can drop the mapping IE entirely and
move both pieces into the FTM action frame.

```
   wired BTC              ESP1 bridge (P4 + C6)               ESP3 (C6 STA)
   ─────────              ─────────────────────                 ─────────────
                           ptpd (port 0, ETH)
   Sync ── eth ──▶  ────▶ Sync RX, BTCA selects upstream
                          BTC, computes correction
                              │
                              ▼  ptpd_register_sync_egress_cb(1, …)
                          esp_ptp publisher task
                              │  (FollowUpInformation marshalled
                              │   from current correctedTime +
                              │   rate/scaledLastGmFreqChange)
                              ▼
                          ESP-Hosted RPC → C6 coprocessor
                              │
                              ▼ esp_wifi_set_vendor_ie(BEACON)
                          C6 radio emits beacon w/ Vendor IE  ─OTA─▶  ESP3 scan_results
                                                                          │
                                                              beacon-IE parser
                                                                          │
                                                              ptpd_inject_sync(0, fu_info, len)
                                                                          │
                                                              FTM measurement → ptpd_inject_peer_delay
                                                                          │
                                                              ptpd servo on port 0 (wifi medium)
                                                              → ptp_clock_sw discipline
```

