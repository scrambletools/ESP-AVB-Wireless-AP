# ESP-AVB-Bridge — implementation plan

## Overview

Two-port AVB bridge on the Waveshare ESP32-P4-WiFi6-PoE-ETH combo
board: P4 host owns the wired AVB segment (EMAC, port 0) and the
onboard C6 coprocessor over SDIO owns the Wi-Fi side as a SoftAP
(port 1). The bridge is intentionally a transparent L2 device — no
ATDECC entity of its own — and acts on AVB control planes (gPTP,
MSRP, MVRP, MAAP) per-port while forwarding ATDECC and AVTP stream
data between the two media.

`esp_avb` is the shared component (endpoint + bridge), `esp_ptp`
provides the gPTP daemon, `esp_ptp_rpc` is the host↔coprocessor
custom-RPC channel used to drive the SoftAP-side Vendor IE
publisher.

## Status

| Subsystem | State |
| --- | --- |
| L2 forwarder (Ethernet ↔ Wi-Fi) | working |
| MRP §10 state machines | working — Applicant (Table 10-3), Registrar (Table 10-4), JoinTimer / LeaveAllTimer / PeriodicTimer |
| MSRP §35 application | working — domain, talker_advertise, talker_failed, listener |
| MVRP §11 application | working |
| MAAP (1722-2016 Annex B) | working |
| Bridge MAP propagation | working — talker + listener + domain, transition-edge gated, decl-change tracked |
| Bridge MAP §35.2.4.4.3 listener merging | working — Ready / AskingFailed / ReadyFailed precedence across ports |
| Bridge MAP §35.2.4.3 Class A → Wi-Fi failure | working — declares TALKER_FAILED with `insufficient_bandwidth_for_traffic_class` |
| Admission control (per-port, per-class) | working — bandwidth tracker in `mrp.c §6`, gates MAP propagation |
| Wi-Fi efficiency cuts | working — LeaveAll jitter widening, STA-presence gating, Class A drop on Wi-Fi egress |
| Per-port topology axes | working — `medium`/`host_if`/`type`/`wifi_mode`/`link_speed_mbps`, all from `esp_ptp` Kconfig |
| ATDECC CLASS_B flag honoring (endpoint side) | working — first listener wins the class on talker, `talker_exclusive` rejects mismatch |
| Bridge forwarding observability | working — heartbeat reports `fwd eth=X/Y wifi=A/B oom=Z STA=N` every 5 s |
| gPTP — wired side (port 0) | working — ptpd locks to MOTU AVB switch BTC, ±40 ns offset |
| gPTP — Wi-Fi side (port 1) | **not implemented** — design captured below (Beacon-IE FollowUpInformation + FTM peer delay). Wi-Fi listeners currently declare themselves BTC. |

## What's implemented

Concise reference for "where does X live?" — section markers in
`mrp.c` are §1/§1b/§6/§6a/§6b/§6c/§7/§8.

| File | Role |
| --- | --- |
| `esp_avb/mrp.c` | MRP SMs + MSRP/MVRP application + MAAP + bridge MAP + admission. Section markers in the header comment. |
| `esp_avb/mrp.h` | Public SM API + wire types + enums. |
| `esp_avb/avb.h` | `avb_state_s`, `ctrl_rx_pkt_t`, NVS persist, codec caps, per-port topology fields. |
| `esp_avb/avtp.h` | 1722 stream payloads (61883, AAF, CRF) + MAAP. |
| `esp_avb/atdecc.h` | 1722.1 ADP/AECP/ACMP/AEM/CVU/MVU + `avtp_msgbuf_u`. |
| `esp_avb/include/esp_avb.h` | Public port-topology enums. |
| `esp_avb/avb.c` | Port-init from Kconfig. `avb_periodic_send` ticks MRP per-port and declares per-class. RX dispatch via single `mrp_rx_msrp` / `mrp_rx_mvrp` call. |
| `esp_avb/avbnet.c` | Unified EMAC + Wi-Fi RX → `avb_unified_rx_cb` → bridge classifier (bridge mode) or `ctrl_rx_queue` (endpoint). `avb_net_send_on(port, …)` for per-port control-plane TX. Ingress port looked up by medium (`s_eth_port_idx` / `s_wifi_port_idx`, cached at init). |
| `esp_avb/atdecc.c` | ACMP CONNECT/DISCONNECT drive `mrp_declare_listener` / `mrp_withdraw_listener` with decl_event from the live SM. |
| `esp_avb/avbbridge.c` | L2 forwarder + SoftAP STA-association count (fed by the bridge application's WIFI_EVENT handlers; read by `mrp_port_tick` for LeaveAll suppression). Classifier verdicts: TERMINATE for PTP/MSRP/MVRP; BRIDGE for ATDECC and VLAN-tagged Class B; DROP for VLAN-tagged Class A egressing Wi-Fi (defense in depth — the MAP layer already refuses to declare the TALKER on that egress). |
| `esp_avb/avbfqtss.c` | 802.1Qav credit-based shaper (bridge build only). |
| `esp_ptp/ptp.c` | ptpd implementation. `ptpd_start_port` is the multi-port API; only `(port=0, medium=ethernet, peer_delay=gptp_wire)` is wired so far. `ptpd_register_sync_egress_cb` / `ptpd_inject_sync` / `ptpd_inject_peer_delay` are surfaces for the future Wi-Fi path. |
| `esp_ptp/ptp_beacon_ie.c` | Host-side Beacon Vendor IE publisher stub. Currently fires once on `WIFI_EVENT_AP_START` with a zero-payload §12.7 FollowUpInformation. To become a periodic publisher driven by the egress callback. |
| `esp_ptp_rpc/src/ptp_custom_rpc.c` | Coprocessor handler for `PTP_RPC_MSG_SET_VENDOR_IE_REQ/ACK`. Unpacks the buffer and calls `esp_wifi_set_vendor_ie()` locally on the C6 (the host's `esp_wifi_remote` stub is a no-op). |
| `ESP-AVB-Bridge/main/avb_bridge_main.c` | App entry. Brings up EMAC + ptpd (port 0), Wi-Fi SoftAP, then `avb_start`. Heartbeat loop reports forwarding stats every 5 s. |
| `ESP-AVB-Bridge/coprocessor/` | C6 firmware = ESP-Hosted network_adapter + custom-RPC handler. |

## What's left

### Critical — required for full bridge functionality

#### 1. gPTP-over-Wi-Fi (multi-week)

The single biggest gap. Without time on the Wi-Fi side, listeners on
the SoftAP have no shared media clock with the wired BTC and Hive
correctly reports them as `grandmaster_id=0`. AVTP streams to Wi-Fi
listeners cannot meet Milan §5.6 presentation budgets.

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
host-side API or by `esp_wifi_remote`. The byte format of the IE is
exactly the §12.7 spec; only the carrier frame type changes. The
practical cost: timing precision is bounded by beacon TX jitter
(roughly the beacon interval, ~100 ms) rather than the much tighter
FTM round-trip timestamps that §12.1.2 was designed around. Revisit
if/when IDF exposes the FTM responder IE — the carrier swap is
wire-compatible by design.

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

Current state of each piece:

| Piece | Where | State |
| --- | --- | --- |
| `ptpd_start_port(port, iface, medium)` | `esp_ptp/ptp.c` | Done — accepts `eth_hwts` (bootstrap) or `wifi_ftm` (attach). |
| Bootstrap init split into `ptp_port_init_eth_hwts` / `_wifi_ftm` | `esp_ptp/ptp.c` | Done — no port-index assumptions; either medium can bootstrap. |
| Bridge calls `ptpd_start_port(1, "WIFI_0", wifi_ftm)` after `init_wifi_softap()` | `ESP-AVB-Bridge/main/avb_bridge_main.c` | Done. |
| Daemon's Sync emit timer drives `sync_egress_cb` on wifi_ftm ports at gPTP cadence | `esp_ptp/ptp.c (ptp_periodic_send)` | Done — fires at `CONFIG_NETUTILS_PTPD_SYNC_INTERVAL_MS`. |
| `ptp_marshal_follow_up_for_beacon_ie()` builds 76-byte §12.7 Follow_Up bytes from ptpd state | `esp_ptp/ptp.c` | Partial — common header (with `sourcePortIdentity` = our own, `flags`, `controlfield`, `logMessageInterval`, gPTP messageType bit), preciseOriginTimestamp from `ptpd_now()`, FollowUpInformation TLV header constants (type, length, OUI 00:80:c2, sub-OUI 00:00:01). Open: `correctionField` accumulation, GM-clockIdentity propagation when synced to upstream, TLV rate/phase/freq fields. |
| `ptp_beacon_ie.c` registers `sync_egress_cb` on AP_START + packs §12.7 IE → Vendor IE | `esp_ptp/ptp_beacon_ie.c` | Done. |
| Coprocessor clear-then-set on every Vendor IE install | `esp_ptp_rpc/ptp_custom_rpc.c` | Done. |
| `PTP_RPC_MSG_SET_VENDOR_IE_REQ/ACK` RPC | `esp_ptp_rpc` | Working transport. |
| STA-side §12.7 Vendor IE parser | `ESP-AVB-Endpoint/main/avb_endpoint.c` `on_vendor_ie` | Done — decodes the 76-byte Follow_Up, validates messagetype, logs GM clockIdentity / sequenceId / correctionField / preciseOriginTimestamp every Nth beacon. Does NOT yet feed into the daemon. |
| `ptpd_inject_sync(port, fu_bytes, len)` daemon-side processor | `esp_ptp/ptp.c` | Still `-ENOSYS`. Wiring the parsed Follow_Up into ptpd's Follow_Up RX state machine is the next blocking task on the STA-clock-discipline path. |
| `ptpd_inject_peer_delay(port, ns)` | `esp_ptp/ptp.c` | API in place, daemon-side servo path not consumed yet. |
| STA-side FTM client | new file in `esp_ptp` | Does not exist. |
| STA-side `ptpd_start_port(0, "WIFI_0", wifi_ftm)` from C6 endpoint main | `ESP-AVB-Endpoint/main` | Not wired. |
| `avb_port_time_source_beacon_ie_wifi` enum | `esp_avb/include/esp_avb.h:80-88` | Declared, not consumed. |

Punch list — remaining work, in dependency order:

1. ~~Implement `ptpd_inject_sync(port, fu_bytes, len)`~~ — **done.**
   Parses 76-byte §12.7 Follow_Up, validates messagetype, synthesises
   `selected_source` from the GM identity, calls
   `ptp_update_local_clock` with t1 = `preciseOriginTimestamp` and
   t2 = `ptpd_now()`. Wired up on the STA via `on_vendor_ie` with
   per-`preciseOriginTimestamp` dedup so each unique IE is processed
   exactly once.

2. ~~Live rate/phase/freq tracking in the marshaller~~ — **design
   decision: keep zero.** Under our "regenerate, don't forward"
   design (see marshaller comment), our published time advances at
   GM rate (the local PI servo absorbs crystal drift before we
   publish), so `cumulativeScaledRateOffset = 0` is semantically
   correct. `gmTimeBaseIndicator` / `lastGmPhaseChange` /
   `scaledLastGmFreqChange` are only meaningful on GM reselection;
   ptpd doesn't track those events yet — add when BTCA-reselection
   plumbing exists. Documented in `ptp_marshal_follow_up_for_beacon_ie`.

3. ~~GM-identity propagation in the marshaller~~ — **done.** When
   `state->selected_source_valid` is true, the Follow_Up header's
   `sourceClockIdentity` is overridden with
   `state->selected_source.btc_identity`. STAs now see the upstream
   wired BTC (verified OTA: ESP3 logs
   `locked onto GM 0001f2fffeff3b14` = MOTU AVB switch in EUI-64).

4. ~~`correctionField` residence-time accumulation~~ — **design
   decision: keep zero.** Our `preciseOriginTimestamp = ptpd_now()`
   is already PI-servo-corrected GM-time, so no residence-time
   accumulation is needed. Documented inline.

5. ~~STA — FTM client~~ — **infrastructure done, measurement
   blocked on hardware.** The FTM client task lives in
   `ESP-AVB-Endpoint/main/avb_endpoint.c` (`ftm_client_task` +
   `WIFI_EVENT_FTM_REPORT` handler); the bridge coprocessor has
   `CONFIG_ESP_WIFI_FTM_ENABLE=y` and
   `CONFIG_ESP_WIFI_FTM_RESPONDER_SUPPORT=y`. `ptpd_inject_peer_delay`
   is implemented and stores into `port->peer_delay_ns` via the same
   running-average shape as the wired Pdelay path.

   At runtime the bridge does respond to the FTM Action frame
   (`wifi:Starting FTM session with <bssid> in N mSec` log appears)
   but every report returns `FTM_STATUS_NO_VALID_MSMT` (status=5).
   Status semantics: the FTM handshake completed and frames were
   exchanged (otherwise we'd get `FTM_STATUS_FAILURE`), but every
   RTT sample in the burst was unusable. Root cause not yet
   investigated on hardware; what we *have* verified:
   - `.ap.ftm_responder = true` is set in the bridge's
     `wifi_config_t.ap` at `init_wifi_softap()` (avb_bridge_main.c).
   - ESP-Hosted RPCs the bit through to the C6
     (`espressif__esp_hosted/host/drivers/rpc/core/rpc_req.c:378`).
   - `CONFIG_ESP_WIFI_FTM_RESPONDER_SUPPORT=y` on the coprocessor;
     `CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT=y` on the endpoint.
   - Initiator cfg matches the IDF FTM example shape exactly
     (`frm_count`, `burst_period`, `channel`, `resp_mac`).

   So today `peer_delay_ns` stays at 0, the static beacon-IE
   pipeline bias (~24–50 ms) is uncompensated, and the STA clock
   tracks to ±50 ms. Next investigation steps when the bias
   matters: (a) log `ap_info.ftm_responder` on the endpoint before
   initiating to confirm the AP advertises FTM in beacons,
   (b) test against an FTM-capable commodity AP to isolate
   bridge-side vs initiator-side, (c) move to the spec-aligned
   §12.7 FTM Vendor IE carrier once IDF exposes the FTM responder
   IE (eliminating the beacon-pipeline lag entirely).

6. ~~STA — enable Wi-Fi medium ptpd on ESP-AVB-Endpoint~~ —
   **done.** `start_wifi_endpoint()` in `avb_endpoint.c` now calls
   `ptpd_start_port(0, "WIFI_STA_DEF", ptp_port_medium_wifi_ftm)`
   after STA association. The daemon's GM-emit and DelayReq paths
   are gated on having an `eth_hwts` port with an open socket, so
   wifi-only endpoints don't spam EBADF.

7. **OUI registration (operational)**. Locally-administered
   `02:00:00` is fine for the development bench but collides with
   anyone else doing the same. Either obtain a Scramble Tools OUI
   from IEEE or coordinate with Espressif on a type code under
   theirs. Non-blocking for bring-up; required before distribution.

8. **STA-side servo audit**. With Sync arriving on the ~100 ms
   beacon cadence (10 Hz) instead of the wired ~8 Hz, plus the
   wider peer-delay variance from FTM, the existing ptpd servo
   gains may need a Wi-Fi-specific tuning block. Verify lock +
   standard deviation on a 30-minute soak before declaring done.

Total remaining scope: items 5 (FTM client) + 7 (OUI) + 8 (servo
audit) — ~120 LOC plus operational/measurement work.

What's done so far on the gPTP-over-Wi-Fi work:

**Phase 1 (minimum viable wireless lock)**
- `ptpd_start_port` is attribute-driven (no port-index special-
  casing), accepts both `eth_hwts` and `wifi_ftm` for bootstrap or
  attach.
- `ptp_initialize_state` split into per-medium init helpers
  (`ptp_port_init_eth_hwts`, `ptp_port_init_wifi_ftm`).
- Daemon's Sync emit timer dispatches `sync_egress_cb` on wifi_ftm
  ports at gPTP cadence.
- `ptp_beacon_ie.c` reworked as a callback consumer; coprocessor
  RPC handler clears any prior Vendor IE before installing the new
  one (fixes IDF "vendor ie has been setted" rejection).
- Bridge's `avb_bridge_main.c` calls `ptpd_start_port(1, "WIFI_0",
  wifi_ftm)` after the SoftAP comes up.
- C6 endpoint's `start_wifi_endpoint()` calls
  `ptpd_start_port(0, "WIFI_STA_DEF", wifi_ftm)` after STA
  association.
- `ptpd_inject_sync()` parses 76-byte §12.7 Follow_Up, validates,
  drives the local clock servo.
- STA-side `on_vendor_ie` decodes the IE and feeds `ptpd_inject_sync`
  with per-`preciseOriginTimestamp` dedup.

**Phase 2 (correctness of the wire bytes)**
- Marshaller publishes upstream BTC's clockIdentity (from
  `selected_source.btc_identity`) instead of the bridge's own MAC.
  STAs now see the wired GM in their GET_RX_STATE.
- `correctionField = 0` and `cumulativeScaledRateOffset = 0` are
  documented design choices under the "regenerate, don't forward"
  approach (preciseOriginTimestamp is already PI-servo-corrected
  GM-time, no additional accounting needed).

**End-to-end validation (Phase 1 + 2):**
- OTA beacons carry a 76-byte §12.7-shaped Vendor IE (verified
  with tshark on mon6).
- ESP3 decodes, dedups, and feeds the bytes into ptpd.
- ESP3 logs `inject_sync port=0: locked onto GM 0001f2fffeff3b14`
  (= MOTU AVB switch's MAC in EUI-64) and `Local time / remote
  time` deltas tracked by `ptp_update_local_clock`.

Validation plan:

- Wireshark on a `mon` interface on channel 6 (the bridge SoftAP)
  — confirm Vendor IE in beacons (OUI + type 0 + payload length),
  then confirm payload bytes match what ptpd publishes on the wired
  Ethernet side at the same instant (tap on the bridge upstream).
- Hive — ESP3's `grandmaster_id` should match the wired BTC's
  clock-identity (today: MOTU 8D or Mac mini depending on which
  wins BTCA).
- ptpd diag on ESP3 — `clock is stablized` with offset_ns < 500
  (Milan §5.6 budget; expect ~100–300 ns from FTM noise on this
  hardware).
- AVTP listener on ESP3 connected to a Class B wired talker —
  audio plays out without sample-rate stretching. The MAP gate
  added today will refuse Class A streams here cleanly with
  TALKER_FAILED.

### Important — spec gaps worth closing

#### 2. `failure_bridge_id` population on TALKER_FAILED

Per §35.2.2.8.5 the bridge's bridge_id is supposed to be carried in
TALKER_FAILED frames so the listener can identify *which* bridge
along the path refused admission. Today we leave it zeroed. Small
fix in `mrp_declare_talker_failed` — populate from the bridge's
clock identity or port-0 MAC.

#### 3. `accumulated_latency` aggregation in MAP

When the bridge propagates a TALKER from port X to port Y, the
attribute should carry the cumulative latency including the
bridge's own forwarding contribution per §35.2.2.8.6. Today we pass
the wire's value through unchanged. Add the per-port forwarding
latency (link-rate + queue worst-case) at propagation time.

#### 4. LeaveTimer event dispatch verification

The Registrar SM correctly handles `mrp_event_leave_timer` (LV →
MT), but the timer-tick path (`mrp_port_tick`) only fires JoinTimer
and the LeaveAllTimer. There's no code that walks each Registrar's
`leave_timer_us` and dispatches `mrp_event_leave_timer` on expiry.
This means a peer that goes silent without `rLv` (e.g., yanked
cable) will sit in our Registrar's IN state forever, holding
admission. Fix: in `mrp_port_tick` walk the per-attribute tables
checking `e->sm.leave_timer_us`.

#### 5. Stream-format change re-admission

If a controller issues `SET_STREAM_FORMAT` that changes the stream's
bandwidth (e.g., 8ch → 16ch, 48k → 96k), the bridge's admission
accounting should release the old allocation and re-admit at the new
bps. Currently `avb_srp_admission_*` is only touched at MAP
propagation time, which is on Registrar transitions — a format
change mid-life doesn't fire one. The bridge MAP block already
detects mid-life class changes (`old_priority != new_priority`); add
a similar branch for bps changes triggered by MSRP TSpec updates.

#### 6. Multi-bridge cascade behaviour

The current bridge MAP propagates 1:1 across its two ports and
accumulates admission per egress, which is correct for a single-hop
bridge. In a multi-bridge AVB network it also has to:

- Honor an upstream `TALKER_FAILED` instead of trying to re-admit
  (don't overwrite a peer-bridge's failure with our own).
- Bound MAP propagation hops via the standard MSRP forwarding
  rules — verify we don't loop in a cycle.

Not blocking on the current bench (single bridge), but worth a
review pass once gPTP-over-Wi-Fi is in.

### Polish / observability

- **SM state introspection diag.** `avbstats.c` already reports RX
  breakdown, per-task CPU, EMAC DMA missed_fc. Add a periodic dump
  of MRP Applicant + Registrar state per (port, attr_type,
  stream_id) — invaluable when peers misbehave.
- **Bridge MAP success/fail counters.** `mrp_declare_*` /
  `mrp_withdraw_*` calls per port + count of admission rejections.
  Heartbeat already shows L2 forwarding; the SM-level activity is
  invisible right now.
- **Long-running soak.** A 1-hour CONNECT/DISCONNECT churn loop
  against MOTU 8D + ESP3 to validate no admission leak, no SM
  desync, no table-full conditions. Probably scriptable from
  `avb_controller.py`.
- **C6 SDIO wedge mitigation.** Recurring failure mode where the
  C6's SDIO link drops after Wi-Fi init under heavy traffic;
  recovery is documented in AGENTS.md but the trigger isn't yet
  understood. Capture the C6 UART during the wedge to see if it
  comes from upstream `wifi:pp q full` or a coprocessor crash.

### Design choices we keep

- **Bridge has no ATDECC entity.** It's a transparent L2 device per
  Milan / 802.1Q semantics. Not adding a Controller-class entity
  unless an operational need appears.
- **Class A AVTP on Wi-Fi is refused at admission time** (Milan
  §5.6 budgets aren't achievable over 802.11). The defense-in-depth
  drop in the L2 forwarder stays as a backstop for non-conformant
  talkers that emit anyway.
- **MAP propagation runs admission per egress, not per ingress**.
  A failed admission on egress port Y produces a TALKER_FAILED on Y
  with `insufficient_bandwidth_for_traffic_class`, leaving the
  ingress-side Registrar untouched. Cleaner than poisoning the
  ingress SM.

## Test environment

### Hardware

- **ESP1** — Waveshare ESP32-P4-WiFi6-PoE-ETH. Bridge target. P4
  host + onboard C6 over 4-bit SDIO. The C6 EN is wired both ways
  (P4 GPIO 54 + ESP-Prog-2 RTS on H7); see AGENTS.md for the safe
  reflash recipe.
- **ESP2** — ESP32-P4-ETH dev board. Wired endpoint regression
  target.
- **ESP3** — ESP32-C6 dev board with ES8311 codec. Wireless
  endpoint, STA to the `ESP-AVB-Bridge` SoftAP.
- **MOTU AVB switch + MOTU 8D + Mac mini** — wired AVB peers /
  Hive controller.

USB serial device names are not stable. Always verify chip identity
with `esptool.py --port <dev> read_mac` before flashing.

### Build / flash recipes

| Target | Recipe |
| --- | --- |
| Bridge host (P4) | `cd ESP-AVB-Bridge && idf.py build && idf.py -p /dev/ttyACM? flash` (hold C6 EN low for the duration — see AGENTS.md) |
| Bridge coprocessor (C6) | `cd ESP-AVB-Bridge && idf.py -C coprocessor build && idf.py -C coprocessor -p /dev/ttyACM? flash` |
| Wired endpoint (P4) | `cd ESP-AVB-Endpoint && idf.py set-target esp32p4 && idf.py -p /dev/ttyACM? flash` |
| Wireless endpoint (C6) | `cd ESP-AVB-Endpoint && idf.py set-target esp32c6 && idf.py -p /dev/ttyACM? flash` |

### Diagnostic tools

- `avb_controller.py` (in `~/Development/tools/`) — discover,
  connect, disconnect, get-tx-state, stream-info, format set.
  `--class-b` on connect requests the talker emit on Class B.
- **Wi-Fi monitor capture** — bring up `mon6` on `phy1` (or `phy0`)
  channel 6:

  ```bash
  sudo -n python3 - <<'PY'
  import subprocess
  subprocess.run(['ip', 'link', 'set', 'wlan1', 'down'])
  subprocess.run(['iw', 'phy', 'phy1', 'interface', 'add', 'mon6',
                  'type', 'monitor', 'flags', 'otherbss'])
  subprocess.run(['ip', 'link', 'set', 'mon6', 'up'])
  subprocess.run(['iw', 'dev', 'mon6', 'set', 'channel', '6', 'HT20'])
  PY
  tshark -i mon6 -w /tmp/ota.pcap
  # ...
  # teardown: iw dev mon6 del && ip link set wlan1 up
  ```

  Decode bridge → STA MSRP attributes:

  ```
  tshark -r /tmp/ota.pcap -n -V \
         -Y "wlan.sa==<bridge_softap_bssid> && llc.type==0x22ea"
  ```

- **Wired-side capture**: the bridge-upstream tap is `enp2s0f2`
  (see AGENTS.md for the current tap mapping).

### Recurring failure modes

- **C6 SDIO wedge on first boot after a P4 reflash**: host sees
  `H_SDIO_DRV: failed to read registers` then auto-reboots in a
  loop. Recovery: hard-reset the C6 via its native USB-CDC
  (`esptool.py --port /dev/ttyACM? --after hard_reset run`) while
  holding the P4 in reset, then release. Documented in AGENTS.md.
- **Hive caches per entity_id**: if the entity model changes, Hive
  may show a stale error. Restart Hive or re-enumerate the entity.

## Picking this up

1. **Read `esp_avb/mrp.c` top to bottom first.** The §1–§8
   sectioning in the file header is the architectural map.
2. **Endpoint mode is the regression baseline.** Don't break ESP2.
   Heartbeat at 1 Hz, ptpd `clock is stablized` with offset_ns <
   100, no over-budget warnings, Hive can enumerate.
3. **Bridge mode is `CONFIG_ESP_AVB_ROLE_BRIDGE`** — derived from
   `esp_ptp` Kconfig (any port `type=bridged` flips the flag).
4. **The MRP SMs are spec-faithful per §10.7.7 / §10.7.8.** Cross-
   check Table 10-3 and Table 10-4 when adding transitions. The
   `[s]` notation in Table 10-3 means optional on shared media —
   our convention is to suppress on the LO → VO transition (already
   done).
5. **`rJoinMt` is a Join action** that transitions the Receiver's
   Registrar to IN. The "Mt" only describes the sender's Registrar
   state. This was a recent fix — easy to get wrong.
6. **Per-port topology axes** are orthogonal:
   `medium × host_if × type × wifi_mode × link_speed_mbps`. All
   sourced from `esp_ptp` Kconfig. `host_if` (emac / ahb / sdio /
   spi / usb / other) is the performance classifier — admission
   caps and Wi-Fi cuts derive from it + `medium`.
7. **Class A on Wi-Fi is refused upstream.** Don't try to admit it
   — the bridge MAP and the data-plane both block it.

## References

- IEEE 802.1Q-2018 §10 (MRP), §11 (MVRP), §35 (MSRP)
- IEEE 802.1AS-2020 (gPTP) — §10 Sync, §11 BTCA, §12 PTP message
  fields, §12.7 FollowUpInformation TLV
- IEEE 1722-2016 (AVTP) — Annex B (MAAP)
- IEEE 1722.1-2021 (ATDECC) — §8.2.1.16 ACMP flags including
  CLASS_B
- Milan v1.3 — §5.4 ACMP, §5.5 Listener decoupling, §5.6 latency
  budgets
- IEEE 802.11mc — FTM
- mrpd (AVnu Alliance, BSD-3) — cleanest open MRP/MSRP reference

Not references: Linux kernel `net/bridge/br_mrp_*.c` is Media
Redundancy Protocol, an unrelated spec.
