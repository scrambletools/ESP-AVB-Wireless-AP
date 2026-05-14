# MRP / MSRP state machine refactor — project plan

## Status

| Phase | Status |
| --- | --- |
| **1 — Generic MRP core + MSRP cutover (endpoint mode)** | **done** — committed as `0eefbcd` in `esp_avb`, validated on ESP2 wired endpoint |
| **2 — Bridge MAP + admission + Wi-Fi cuts** | next |
| 3 — Real MSRP listener state driver | pending |
| 4 — MVRP on the MRP core | pending |

`esp_avb` HEAD: `0eefbcd feat: MRP state machines + header split`
on top of `5aa5be5 feat: L2 bridge` (commit not yet pushed).

## Goal (unchanged)

Replace `esp_avb`'s former "stimulus → immediate response" MSRP code
with a proper IEEE 802.1Q-2018 §10 (MRP) + §35 (MSRP) state machine
implementation. End state: a generic MRP core (Applicant + Registrar
+ timers + LeaveAll) driving MSRP and MVRP, with bridge-mode MAP
propagation and endpoint-mode origination.

Phase 1 delivered the MRP core + MSRP cutover for **endpoint** mode.
Phase 2 adds bridge-mode MAP that propagates Registrar transitions
across ports and gates them through admission.

## Phase 1 — what landed

Committed in `esp_avb` as **`0eefbcd feat: MRP state machines + header split`** (one squashed commit). Key pieces:

### Header split

`avb.h` shrank from 3192 → 948 lines. Protocol types moved to:

- **`mrp.h`** (411 lines) — MRP / MSRP / MVRP wire types, enums, SM
  types, and the public SM-driven API (`mrp_declare_*`,
  `mrp_withdraw_*`, `mrp_rx_msrp`, `mrp_port_init`, `mrp_port_tick`,
  `mrp_applicant_step`, `mrp_registrar_step`).
- **`avtp.h`** (292 lines) — pure 1722 stream payloads (61883, AAF,
  CRF) + MAAP.
- **`atdecc.h`** (1872 lines) — 1722.1 ADP/AECP/ACMP/AEM/CVU/MVU +
  `avtp_msgbuf_u` envelope union (lives here because it embeds ATDECC
  payload types).

Wire structs remain memcpy-compatible with the L2 frame layout. No
encode/decode pass was introduced — bit-fields stay within single
octets, multi-byte fields use `uint8_t[N]` byte arrays.

### MRP state machines

In `esp_avb/mrp.c` (renamed from `msrp.c`):

| Section | Contents |
| --- | --- |
| §1   | `mrp_applicant_step` (12 states, full §10.7.7 Table 10-3); `mrp_registrar_step` (3 states, §10.7.8 Table 10-4). LO → VO sL suppressed (§10.7.7 `[s]` optional on shared media). |
| §1b  | Per-port timers (§10.7.11): `JoinTimer` (200ms + jitter), `LeaveAllTimer` (10–15s jittered), `PeriodicTimer` (1s). `mrp_port_init` / `mrp_port_tick` / `mrp_port_arm_join_timer`. |
| §6a  | `mrp_rx_msrp` walks an MSRP buffer, decodes 3pe vectors, dispatches peer events to both Applicant + Registrar SMs, then runs the legacy application reactions (`avb_process_msrp_*`) per attribute. |
| §6b  | `mrp_declare_*` / `mrp_withdraw_*` — local origination entry points, drive the Applicant via New!/Join!/Lv!. Idempotent: New! on first call, Join! on already-active SMs. |
| §6c  | `mrp_tx_flush_port` — fires tx! into each Applicant, encodes pending TX actions as single-attribute MRPDUs. sJ resolves to JoinIn (Registrar IN) or JoinMt (MT) at encode time. |
| §6 attribute tables | `s_msrp_talkers[NUM_PORTS][N]`, `s_msrp_listeners[..]`, `s_msrp_domains[..]`. Talker entries keyed by `(stream_id, attr_type)` so ADVERTISE and FAILED for the same stream_id are distinct attributes. |

### Cutover

- `avb.c`'s periodic loop no longer calls `avb_send_msrp_*`.
  `mrp_declare_*` calls instead; the Applicant SM owns retransmission
  cadence.
- The legacy LeaveAll re-declaration block is gone — the SM's
  per-port `LeaveAllTimer` dispatches rLA into all SMs on the port.
- `avb_process_rx_message` no longer dispatches MSRP — it makes a
  single `mrp_rx_msrp(state, port, msg, len, src)` call. The legacy
  `avb_process_msrp_*` reaction functions are called from inside
  `mrp_rx_msrp` per attribute.
- Dead legacy origination functions deleted from `mrp.c`:
  `avb_send_msrp_domain`, `avb_send_msrp_talker`,
  `avb_send_msrp_listener`. Declarations removed from `mrp.h`.
- `atdecc.c` ACMP connect/disconnect rewired:
  `avb_send_msrp_listener(...)` → `mrp_declare_listener(...)` for
  CONNECT; `mrp_withdraw_listener(...)` for DISCONNECT.

### Plumbing

- `ingress_port` added to `ctrl_rx_pkt_t` and surfaced via a new
  `avb_net_recv_ctrl` out-param.
- `state->rxport[AVB_NUM_PROTOCOLS]` tracks per-protocol ingress —
  populated by `avb.c`'s main loop, consumed by `mrp_rx_msrp` so the
  SMs receive events on the right port. Needed for bridge MAP.
- `avb_state_s` gained a tag (`typedef struct avb_state_s { ... }`)
  so the new headers can forward-declare it.

### Validation

Flashed to ESP2 (wired P4 endpoint, MAC `e8:f6:0a:e0:92:20`,
`/dev/ttyACM?` — identify by MAC). 30-second soak result:

- ptpd locked, ±40 ns offset, "clock is stablized"
- Heartbeat steady 1 Hz
- 1487+ ptpd lines, no panics, no asserts, no over-budget warnings
- Zero MSRP send failures in the log
- Shadow-mode pre-cutover (`s_mrp_tx_shadow=true`) showed expected
  SM activity: talker_adv sJ at ~0.5 Hz (resolves to JoinMt because
  Registrar MT — no peer echoes our own talker_advertise back),
  domain sJ similar, observer-state Leave events on LeaveAll cycle

## Phase 2 — bridge MAP + admission + Wi-Fi cuts

### Scope

Bridge mode: cross-port propagation of MRP attribute state, gated by
per-egress admission, with Wi-Fi-specific efficiency cuts.

```
peer A on port[0]           peer B on port[1]
       │                            ▲
       ▼                            │
  Registrar(port=0)  ───MAP──▶  Applicant(port=1)
                       │
                       └─ admission check on egress
                          (avb_srp_admission_try_admit)
                          if fail: declare TALKER_FAILED instead
```

### Concrete to-do

1. **`on_registrar_change` callback hook** — fire only on Registrar
   IN↔MT transitions (not on every RX). Currently the legacy
   `avb_process_msrp_*` reactions run unconditionally per RX. For
   MAP to work correctly we need transition-edge semantics.
   - Add `mrp_registrar_state_e old_reg = e->sm.registrar;` capture
     before `mrp_sm_step`, compare after, call dispatch.
   - Dispatch by attribute type into MSRP-specific bridge handlers.

2. **MAP propagation** — in a new `mrp_map_propagate(port, attr, evt)`
   (likely in `mrp.c` §6c or a new `avbmap.c` co-located with
   `avbbridge.c`). Wrapped in `#ifdef CONFIG_ESP_AVB_ROLE_BRIDGE`.
   - On peer-port-X Registrar `IN`: for each other port Y, call
     `mrp_declare_talker_advertise(state, Y, ...)` (or FAILED if
     admission fails).
   - On peer-port-X Registrar `MT` / LV→MT: call
     `mrp_withdraw_talker(state, Y, stream_id)`.
   - Similar for LISTENER going the other direction.

3. **Admission integration** — re-use existing
   `avb_srp_admission_*` (already in `mrp.c`; was at lines 820–890
   pre-rename, now near the bottom of mrp.c §6). Call
   `avb_srp_admission_try_admit(egress_port, sr_class, bps)` before
   propagating. On failure: `mrp_declare_talker_failed` with
   `failure_code = insufficient_bandwidth_for_traffic_class`.

4. **Wi-Fi efficiency cuts** (all gated by `port[p].medium == wifi`,
   runtime, not Kconfig):
   1. Don't propagate Class A `TALKER_ADVERTISE` → Wi-Fi at all
      (strict spec would propagate as FAILED; we skip — no Wi-Fi
      listener can act on it).
   2. Widen `LeaveAllTimer` jitter on Wi-Fi-medium ports beyond the
      spec's 10–15s when the table is large.
   3. Suppress LeaveAll on Wi-Fi while no STA has any MSRP
      registration; re-enable on first STA join.

5. **`avb_net_send_on(port, ethertype, msg, len)`** — per-port TX.
   Currently `avb_net_send` hard-codes `port[0].l2if[MSRP]`. For
   bridge the SM TX flush needs to ship to the right port.
   Wraps `avb_net_send` for port 0; for port 1 (Wi-Fi) it routes
   through the existing Wi-Fi TX path (see how `avbbridge.c`
   forwarder does it today).

### Approximate LOC

~150 LOC total: ~80 for MAP, ~30 for admission integration, ~20 for
Wi-Fi cuts, ~20 for `avb_net_send_on`.

### Validation plan

- ESP1 (bridge) on the wired AVB switch, with MOTU 8D + Mac mini as
  wired peers and ESP3 as Wi-Fi listener.
- Before Phase 2: bridge currently noisy with `MSRP TALKER over
  budget` warnings ~1 Hz (the Phase 0 stopgap residue). After Phase
  2: warnings gone, ESP3 sees correct propagated declarations on
  the Wi-Fi side (Class B only).
- Wire capture on `enp2s0f?` taps (see Test environment below) +
  Wi-Fi mon3 capture on channel 6.

## Phase 3 — real MSRP listener state driver

Currently `mrp_declare_listener` is always called with
`msrp_listener_event_ready`. That's a placeholder; the real listener
decl_event should reflect actual stream connection state:

| Stream state | Listener declaration |
| --- | --- |
| ACMP CONNECT_TX pending, talker not yet found | `AskingFailed` |
| ACMP CONNECT_TX done, MSRP path open, AVTP flowing | `Ready` |
| ACMP CONNECT_TX done, but talker FAILED upstream | `ReadyFailed` |

This lives in `atdecc.c` (ACMP CONNECT/DISCONNECT handlers) and is
the missing piece for Milan §5.5 listener-state semantics. ~100 LOC.

## Phase 4 — MVRP on the MRP core

Currently MVRP is still on the legacy direct-call path:
`avb_send_mvrp_vlan_id` in `mrp.c`, called from `avb.c`'s periodic
loop every 500 ms. To move it onto the generic SMs:

- Add `mvrp_vlan_entry_t` table (per-port, keyed by VLAN ID), with
  embedded `mrp_sm_state_t`.
- Add `mrp_declare_vlan(state, port, vlan_id)` / `mrp_withdraw_vlan`.
- Hook MVRP RX through `mrp_rx_mvrp(state, port, msg, len, src)`
  parallel to `mrp_rx_msrp`. Avbnet.c's `avb_process_rx_message`
  MVRP branch becomes a single call.
- Extend `mrp_tx_flush_port` to also flush MVRP entries.
- Delete legacy `avb_send_mvrp_vlan_id` / `avb_process_mvrp_vlan_id`.

MVRP has no admission control and no per-class semantics, so it's
much simpler than MSRP. ~50 LOC.

## Outstanding polish (small, can land any time)

1. **Migrate legacy `avb_process_msrp_*` reactions to on_registrar_change
   callbacks** — currently called from inside `mrp_rx_msrp` on every
   RX. Phase 2 prerequisite. After Phase 2, the legacy functions
   can be deleted entirely.
2. **Wi-Fi LeaveAll jitter widening and STA-presence gating** — listed
   under Phase 2 but trivially independent.
3. **`avb_send_msrp_attr` cleanup** — still in `mrp.c`, used by
   `mrp_tx_flush_*`. Could be inlined or renamed to `mrp_send_attr`.
4. **MSRP attribute aggregation on bridge** — when multiple
   downstream listeners exist per §35.2.4.4.3, the bridge merges
   their declarations into one upstream LISTENER on the talker-facing
   port. Not yet implemented; needed for full Milan compliance but
   not for basic AVB.
5. **`s_mrp_tx_shadow` flag** is in tree but always `false` —
   leftover from Phase 1 shadow-mode validation. Can be removed or
   left as a debug knob.
6. **Rename detection** — the Phase 1 commit shows `msrp.c` deletion
   + `mrp.c` creation rather than a rename (content similarity
   dropped below git's 50% threshold from the SM additions).
   `git log --follow mrp.c` still traces history. Cosmetic only.

## Code locations after Phase 1

Files renamed / created / heavily modified:

| File | Role |
| --- | --- |
| `esp_avb/mrp.c` (was `msrp.c`) | MRP SMs (§1–§1b), MSRP application (§6), MAAP (§8 historical). The SM-driven entry points are at §1b and §6a/b/c. Legacy `avb_process_msrp_*` and `avb_send_msrp_attr` still live here as helpers/reactions. |
| `esp_avb/mrp.h` | Public SM API: `mrp_applicant_step`, `mrp_registrar_step`, `mrp_port_init/tick/arm_join_timer`, `mrp_rx_msrp`, `mrp_declare_*`, `mrp_withdraw_*`. Plus the wire types and enums. |
| `esp_avb/avb.h` | Now ~948 lines. Holds `avb_state_s`, `ctrl_rx_pkt_t` (with `ingress_port` field), `avb_msgbuf_u`, NVS persist types, codec caps. Includes `mrp.h`, `avtp.h`, `atdecc.h` transitively. |
| `esp_avb/avtp.h` | 1722 stream payloads + MAAP. |
| `esp_avb/atdecc.h` | 1722.1 + MVU/CVU + `avtp_msgbuf_u`. |
| `esp_avb/avb.c` | `avb_initialize_state` calls `mrp_port_init` per port. `avb_periodic_send` calls `mrp_port_tick` and the SM-driven `mrp_declare_*` instead of legacy `avb_send_msrp_*`. RX dispatch is one `mrp_rx_msrp` call. |
| `esp_avb/avbnet.c` | EMAC RX populates `ctrl_rx_pkt_t.ingress_port`. `avb_net_recv_ctrl` exposes it via an out-param. |
| `esp_avb/atdecc.c` | ACMP CONNECT/DISCONNECT rewired to `mrp_declare_listener` / `mrp_withdraw_listener`. |
| `esp_avb/avbbridge.c` | Unchanged in Phase 1. Phase 2 MAP work likely lands here or in a new `avbmap.c`. |
| `esp_avb/CMakeLists.txt` | `mrp.c` replaces `msrp.c` in the source list. |

Helpful pre-existing infrastructure for Phase 2:

- `avb_srp_admission_*` (per-port-per-class running-bandwidth
  tracker) — in `mrp.c`, gated `#ifdef CONFIG_ESP_AVB_ROLE_BRIDGE`.
- `avb_bridge_classify` (L2 forwarding disposition) — in
  `avbbridge.c`. MAP is a sibling concept.
- `ingress_port` plumbing — already present, ready to consume.

## Reference implementations (don't port — license + style)

- **mrpd** (AVnu Alliance, BSD-3): cleanest open MRP/MSRP/MVRP/MMRP
  implementation. Linux userspace daemon. Best reference for the
  Table 10-3 / 10-4 state tables and MAP semantics.
- **OpenAVB** (GitHub: AVnu/OpenAvnu): includes mrpd + related code.
- **Linux kernel `net/bridge/br_mrp_*.c`** — *not* a reference. That
  MRP is Media Redundancy Protocol, a completely different spec.

## Test environment

### Hardware (per AGENTS.md)

- **ESP1** = Waveshare ESP32-P4-WiFi6-PoE-ETH. **Phase 2 bridge
  target.** P4 host + onboard C6 coprocessor via 4-bit SDIO. P4
  EMAC on `/dev/ttyACM?`, C6 debug UART on a separate `/dev/ttyACM?`
  via the H7 header (needs an external USB-UART like ESP-Prog-2).
  USB device names are not stable; identify by `esptool.py read_mac`
  before flashing.
- **ESP2** = ESP32-P4-ETH dev board. **Phase 1 endpoint validation
  target (already passed).** MAC `e8:f6:0a:e0:92:20`.
- **ESP3** = ESP32-C6 dev board. Wireless AVB endpoint. Wi-Fi STA to
  `ESP-AVB-Bridge` SoftAP. MAC `fc:01:2c:fd:e5:94`.
- **MOTU AVB switch + MOTU 8D + Mac mini (Hive)** = wired AVB peers.

### Build / flash recipes

| Target | Recipe |
| --- | --- |
| Bridge host (P4) | `cd ESP-AVB-Bridge && idf.py build && idf.py -p /dev/ttyACM? flash` |
| Bridge coprocessor (C6) | `cd ESP-AVB-Bridge && idf.py -C coprocessor build && idf.py -C coprocessor -p /dev/ttyACM? flash` (hold P4 in reset via DTR while flashing) |
| Wired endpoint (P4) | `cd ESP-AVB-Endpoint && idf.py set-target esp32p4 && idf.py -p /dev/ttyACM? flash` |
| Wireless endpoint (C6) | `cd ESP-AVB-Endpoint && idf.py set-target esp32c6 && idf.py -p /dev/ttyACM? flash` |

### Diagnostic tools

- **`avb_controller.py`** (in `~/Development/tools/`) — has a
  `read-descriptor` subcommand. Use to directly read Entity /
  Configuration / etc. from a target entity. Bypasses Hive caching.
- **Wi-Fi monitor capture** — host has a USB Wi-Fi NIC (`wlan3`,
  MT7921U driver, 2.4 GHz). Capture over-the-air traffic on channel
  6 (the bridge SoftAP):

  ```bash
  sudo -n python3 -c "
  import subprocess
  subprocess.run(['iw','phy','phy3','interface','add','mon3','type','monitor'])
  subprocess.run(['ip','link','set','wlan3','down'])
  subprocess.run(['ip','link','set','mon3','up'])
  subprocess.run(['iw','dev','mon3','set','channel','6'])"
  tshark -i mon3 -n -Y 'wlan.addr==fc:01:2c:fd:e5:94' -T fields \
    -e frame.time_relative -e wlan.sa -e wlan.da -e _ws.col.Info
  # teardown:
  sudo -n python3 -c "
  import subprocess
  subprocess.run(['ip','link','set','mon3','down'])
  subprocess.run(['iw','dev','mon3','del'])
  subprocess.run(['ip','link','set','wlan3','up'])"
  ```

  Doesn't interfere with internet Wi-Fi (`wlan2` is the 5 GHz primary).
- **Wired-side capture**: `tshark -i enp2s0f1 -n -Y 'eth.type==0x22ea'`
  to watch MSRP from ESP2 upstream; `enp2s0f0` for MOTU 8D upstream;
  `enp2s0f2`/`f3` for switch-side downstream (see AGENTS.md for tap
  topology).

### Recurring failure modes

- **C6 wedge under MSRP flood**: `wifi:pp q full` on C6 UART, then
  host sees `transport: Not able to connect with ESP-Hosted slave
  device` and self-reboots in a loop. Recovery: hold ESP3 in reset
  (or unplug), hold P4 in reset, reflash C6 via `/dev/ttyACM?`,
  release P4. Phase 1 verified this is no longer triggered by our
  own traffic; Phase 2 should keep it that way (the SM-driven path
  is bounded by `PeriodicTimer` 1 Hz × N attributes, not by RX rate).
- **Hive caches per entity_id**: if the entity model changes, Hive
  may show a stale error. Restart Hive or right-click → re-enumerate.
- **`build/` directory**: untracked in `esp_avb`; the repo currently
  has no `.gitignore`. Don't `git add` it.

## What to know if you're picking this up

1. **Read `esp_avb/mrp.c` top-to-bottom first.** The §1–§8 sectioning
   in the file header is the architectural map. §1 (SMs) and §6
   (MSRP application) are mode-agnostic; Phase 2 lands in §6c
   (MAP) and/or a new file.

2. **Endpoint mode is the regression baseline.** Don't break ESP2 +
   Mac mini / MOTU peering. Flash ESP2 after each change, look for:
   - Heartbeat at 1 Hz
   - ptpd "clock is stablized" with offset_ns < 100
   - No `over budget` warnings
   - Hive can still enumerate the entity

3. **Bridge mode is `CONFIG_ESP_AVB_ROLE_BRIDGE` builds** —
   `cd ESP-AVB-Bridge && idf.py build`. The ESP-AVB-Bridge project
   is preset for the P4 bridge build. Endpoint vs bridge is derived
   from `esp_ptp` Kconfig (see `Kconfig.projbuild` in `esp_avb`).

4. **The MRP SMs are spec-faithful per §10.7.7 / §10.7.8.** If
   adding transitions, cross-check Table 10-3 and Table 10-4. The
   `[s]` notation in Table 10-3 means optional on shared media —
   our convention is to suppress (already done for LO → VO).

5. **The legacy `avb_process_msrp_*` reactions still run on every
   RX (not just transitions)** because they were not refactored
   into Registrar-transition callbacks during Phase 1. That's the
   first cleanup before MAP can hook in cleanly. See
   "Outstanding polish" #1 above.

6. **`s_mrp_tx_shadow` is a runtime debug knob** in `mrp.c` (always
   `false` in tree). Flip to `true` if you want SM activity logged
   without it actually transmitting — useful for diffing expected
   vs actual behavior during Phase 2 bring-up.

## Recent context (carried forward from earlier sessions)

- **Phase 5b L2 forwarder** in the bridge (Eth ↔ Wi-Fi). Required
  `ETH_CMD_S_ALL_MULTICAST` in addition to `ETH_CMD_S_PROMISCUOUS`
  on the EMAC (the IDF "promiscuous" only covers unicast). Also
  required overriding `esp_wifi_remote_channel_rx` from `avbnet.c`
  because the registry-resolved `esp_wifi_internal_reg_rxcb` is a
  silent no-op against `esp_wifi_remote_net2.c`.
- **`esp_ptp_rpc`** is a registered component
  (`scrambletools/esp_ptp_rpc`). The bridge's coprocessor build
  pulls it via a tiny stub manifest at
  `coprocessor/components/registry_deps/`.
- **Kconfig unification under `esp_ptp`**: `ESP_AVB_NUM_PORTS`,
  `ESP_AVB_PORT?_*`, `ESP_AVB_ROLE_*` are *derived* from
  `ESP_PTP_*`. Single source of truth.
- **Terminology**: code + comments use **BTC** (best timetransmitter
  clock) instead of grandmaster/GM, **BTCA** instead of BMCA,
  **coprocessor** instead of slave. See Terminology sections in
  `esp_avb/README.md` and `esp_ptp/README.md`.
- **Bridge coprocessor setup**: `setup-coprocessor.sh` clones
  upstream `esp-hosted-mcu` into `./.deps/` (or uses
  `ESP_HOSTED_MCU_DIR`) and wires three relative symlinks. Host
  build needs no manual setup — all deps come from the IDF Component
  Registry.
- **C6 wireless Class B workaround** (still in tree, pre-Phase-1):
  ESP3 declares its streams as Class B on Wi-Fi medium so the
  bridge's v1 admission policy accepts them. With Phase 2 MAP doing
  the right thing this is technically redundant but harmless;
  defer removal until Phase 2 is validated.
