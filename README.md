# ESP AVB Bridge

An Ethernet ↔ Wi-Fi AVB L2 bridge. The bridge presents a SoftAP that
wireless AVB endpoints associate to, and transparently relays AVB
control + stream traffic between its Wi-Fi side and a wired AVB switch.
Time-of-day is propagated to wireless endpoints through the AP's
802.11 beacon Vendor IE, carrying byte-identical IEEE 802.1AS-2020
§12.7 `FollowUpInformation`.

The pairing target is the wireless build of
[scrambletools/ESP-AVB-Endpoint](https://github.com/scrambletools/ESP-AVB-Endpoint),
but any Milan-compatible AVB endpoint on the wired side or any STA on
the SoftAP that speaks AVB-over-Wi-Fi will interoperate.

## issues

- forwarding some unicast traffic from the wired side to the wireless is unstable
- this is software is in early development, intended for limited testing only

## Capabilities

- Transparent L2 forwarder for AVTP control (ADP / AECP / ACMP / MAAP),
  MSRP, MVRP, and VLAN-tagged AVB stream frames in both directions.
- FQTSS Credit-Based Shaper and MSRP admission control on the Wi-Fi
  egress side. Wi-Fi admits Class B only in v1; Class A reservations
  propagating from the wired side are rejected with
  `insufficient_bandwidth_for_traffic_class`.
- gPTP BTC / boundary clock on the wired side via `esp_ptp`'s
  hardware-clock backend on the P4 EMAC.
- Beacon Vendor IE publisher for `FollowUpInformation` on the SoftAP,
  installed via custom RPC over SDIO to the coprocessor — no host
  application code needs to touch the radio.
- 802.11 FTM responder on the SoftAP so wireless endpoints can measure
  peer-delay).
- No ATDECC entity of its own — the bridge is L2-transparent per Milan
  / 802.1Q semantics.

## Hardware

Target board: **Waveshare ESP32-P4-WiFi6-PoE-ETH**.

- **ESP32-P4** (host) — owns the wired Ethernet PHY (IP101), runs the
  AVB stack and gPTP master, and dispatches the Wi-Fi side over SDIO
  through `esp_wifi_remote` / `esp_hosted`.
- **ESP32-C6** (onboard coprocessor) — Wi-Fi radio, reached over
  4-bit SDIO. Runs the upstream ESP-Hosted slave firmware unmodified,
  overlaid with the `esp_ptp_rpc` handler that the host calls into to
  install the beacon Vendor IE.
- SDIO wiring (4-bit, slot 1, from the board schematic):

  | Signal | P4 pin | C6 pin |
  | --- | --- | --- |
  | CLK | GPIO18 | GPIO19 |
  | CMD | GPIO19 | GPIO18 |
  | D0  | GPIO14 | GPIO20 |
  | D1  | GPIO15 | GPIO21 |
  | D2  | GPIO16 | GPIO22 |
  | D3  | GPIO17 | GPIO23 |

  CMD/CLK appear "swapped" because both ICs place those signals on
  each other's nominal pins. The schematic net names follow the P4
  host pin numbers. ESP-Hosted's P4 defaults already match this
  wiring, so the project does not override pins.
- **C6 reset** is driven by P4 GPIO54 (wired to the coprocessor's EN
  line via the board). The host resets the coprocessor at boot to
  start the SDIO handshake from a known state.

## Repository layout

```
ESP-AVB-Bridge/
  main/                  P4 host application (AVB stack + SoftAP
                         setup). idf_component.yml pulls
                         scrambletools/esp_avb and esp_ptp from the
                         ESP Component Registry; esp_ptp_rpc comes in
                         transitively.
  sdkconfig.defaults     P4 host defaults (target=esp32p4, AVB role,
                         esp_ptp port topology, ESP-Hosted SDIO
                         config).
  components/
    sdio_avb_link/       Bridge-local SDIO link helpers.

  coprocessor/           Coprocessor (C6) firmware build root.
    CMakeLists.txt       Mirrors upstream esp-hosted-mcu/slave but
                         pulls in esp_ptp_rpc + chains our defaults.
    sdkconfig.defaults   Coprocessor overrides
                         (CONFIG_PTP_RPC_BUILD_COPROCESSOR_HANDLER=y).
```

No manual setup is required for the host build — the AVB / gPTP /
RPC components are managed dependencies and idf.py installs them
into `managed_components/` automatically on first build. The
coprocessor build has one additional prerequisite (cloning upstream
`esp-hosted-mcu` source); see *Building and flashing* below.

## Building and flashing

The bridge runs **two separate firmwares** — one on the P4 host, one
on the onboard C6 coprocessor. Both must be flashed for the bridge to
come up. The flash order does not matter; the host resets the
coprocessor over GPIO54 at boot and the SDIO handshake will succeed
once both sides are in place.

### 1. Coprocessor (C6) firmware

The coprocessor runs Espressif's upstream ESP-Hosted slave firmware
overlaid with the `esp_ptp_rpc` handler. The slave is not distributed
as a registry component (it's its own project mounted via local
symlinks), so a one-time setup script fetches it and wires the
symlinks in:

```
./setup-coprocessor.sh
```

By default the script clones the upstream source into `./.deps/`
(gitignored). To point at a pre-existing checkout instead, override
via env var:

```
ESP_HOSTED_MCU_DIR=~/src/esp-hosted-mcu ./setup-coprocessor.sh
```

The script is idempotent — re-running it just refreshes the symlinks.

`esp_ptp_rpc` is a registry component. The stub manifest at
`coprocessor/components/registry_deps/` declares it so the IDF
Component Manager pulls it into `managed_components/` automatically
on first build. (The stub exists because the coprocessor's `main/`
is symlinked to upstream `slave/main` and we can't add a manifest
there.) Developers working on `esp_ptp_rpc` locally can place a
symlink at `coprocessor/components/esp_ptp_rpc` — that path is
gitignored and takes precedence over the managed copy.

Build and flash via the C6's debug UART on the H7 header (auto-reset
wired):

```
idf.py -C coprocessor set-target esp32c6
idf.py -C coprocessor build
idf.py -C coprocessor -p /dev/<serial-device> flash
```

The build chains three sdkconfig defaults files:

1. upstream `esp-hosted-mcu/slave/sdkconfig.defaults`
2. upstream `esp-hosted-mcu/slave/sdkconfig.defaults.esp32c6`
3. `coprocessor/sdkconfig.defaults` (our overrides — most importantly
   `CONFIG_PTP_RPC_BUILD_COPROCESSOR_HANDLER=y`, which compiles in the
   handler that responds to `PTP_RPC_MSG_SET_VENDOR_IE_REQ` from the
   host)

### 2. Host (P4) firmware

Build root: project root.

```
cd ESP-AVB-Bridge
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/<serial-device> flash monitor
```

Key host-side defaults baked into `sdkconfig.defaults`:

- `CONFIG_IDF_TARGET="esp32p4"`, P4 silicon rev v1.3 settings
- `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y`, slot 1, 4-bit bus
  (matches the schematic wiring above, no pin overrides needed)
- `CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y` — the additive RPC
  channel that carries the Vendor IE install request to the C6
- `CONFIG_ESP_PTP_NUM_PORTS=2` with `PORT0=ethernet+bridged` and
  `PORT1=wifi_cp+bridged+ap`. The matching `esp_avb` symbols
  (`NUM_PORTS=2`, `ROLE_BRIDGE`, etc.) are derived automatically — do
  not set them by hand.
- AVB Lite advertised on the wired side
  (`CONFIG_ESP_AVB_AVB_LITE_COMPLIANT=y`)

### Identifying which `/dev/<serial-device>` is which

USB device names are not stable across host or device restarts.
Identify by chip ID before flashing:

```
esptool.py --port /dev/<serial-device> read_mac    # → "Chip type: ESP32-P4" or ESP32-C6
```

## Standards

- IEEE 802.1Q-2022 (FQTSS Credit-Based Shaper, MRP/MSRP/MVRP)
- IEEE 802.1AS-2021 (gPTP, including §12.7 `FollowUpInformation`)
- IEEE 1722-2016 (AVTP)
- IEEE 802.11 (Wi-Fi SoftAP, FTM peer-delay, beacon Vendor IE)

## Troubleshooting

- **Boot loop with `transport: Not able to connect with ESP-Hosted
  slave device` on the host UART.** The C6 coprocessor isn't
  responding to the SDIO handshake. Most often the C6's Wi-Fi
  packet-processing queue has saturated and the C6 firmware has wedged.
  Recovery: hold the P4 in reset (so it stops pulsing GPIO54), then
  re-flash the C6 via `idf.py -C coprocessor flash`, then release the
  P4.
- **SoftAP is up but the wireless endpoint never sees the bridge's
  Vendor IE.** Check that the C6 firmware was built with
  `CONFIG_PTP_RPC_BUILD_COPROCESSOR_HANDLER=y` (the override in
  `coprocessor/sdkconfig.defaults`). Without it the install request
  from the host is silently dropped on the coprocessor side.
- **Wired controller doesn't see frames from a wireless endpoint
  associated to the bridge.** Verify the host log shows
  `Bridge L2 forwarder armed: Eth(port0) <-> Wi-Fi-AP(port1)`. If
  only PTP frames arrive at the EMAC RX callback (visible in the
  periodic AVB stats panel), check that
  `ETH_CMD_S_ALL_MULTICAST` succeeded — without it the GMAC frame
  filter drops AVTP and MVRP multicast destinations.

## Open source

This bridge application and the underlying `esp_avb` / `esp_ptp` /
`esp_ptp_rpc` components were initially developed by Scramble Tools
LLC and are released under the MIT license to encourage AVB-over-Wi-Fi
experimentation across the community.

## Feedback

Please file issues or pull requests via the Github repository.
