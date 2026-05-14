#!/usr/bin/env bash
# One-time setup for the ESP-AVB-Bridge coprocessor (C6) firmware build.
#
# The coprocessor runs Espressif's upstream ESP-Hosted slave firmware,
# which is not distributed as an IDF Component Registry package — it
# is mounted into the coprocessor build via local symlinks. This
# script fetches that upstream source and wires the symlinks in.
#
# (esp_ptp_rpc, by contrast, is a registry component pulled by the
# stub manifest at coprocessor/components/registry_deps/. Developers
# who want to edit it locally can place a symlink at
# coprocessor/components/esp_ptp_rpc — that path is gitignored and
# overrides the managed copy.)
#
# Idempotent — safe to re-run. The host (P4) firmware does not need
# this script; its dependencies all come from the registry via
# main/idf_component.yml.
#
# Env-var override (use a pre-existing checkout):
#   ESP_HOSTED_MCU_DIR  path to a local esp-hosted-mcu clone

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

DEPS_DIR="$HERE/.deps"
mkdir -p "$DEPS_DIR"

ESP_HOSTED_MCU_DIR="${ESP_HOSTED_MCU_DIR:-$DEPS_DIR/esp-hosted-mcu}"

if [ -d "$ESP_HOSTED_MCU_DIR/.git" ] || [ -d "$ESP_HOSTED_MCU_DIR/slave" ]; then
  echo ">> esp-hosted-mcu found at $ESP_HOSTED_MCU_DIR"
else
  echo ">> Cloning esp-hosted-mcu into $ESP_HOSTED_MCU_DIR..."
  git clone https://github.com/espressif/esp-hosted-mcu.git "$ESP_HOSTED_MCU_DIR"
fi

# Resolve to absolute path in case the env var input was relative.
ESP_HOSTED_MCU_DIR="$(cd "$ESP_HOSTED_MCU_DIR" && pwd)"

# Create $link_path as a symlink whose target is $target_abs expressed
# relative to the link's directory, so the link survives the tree
# being moved as long as the .deps/ contents move with it.
link_relative() {
  local link_path="$1" target_abs="$2"
  local link_dir
  link_dir="$(dirname "$link_path")"
  mkdir -p "$link_dir"
  rm -f "$link_path"
  local rel
  rel="$(python3 -c \
    "import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))" \
    "$target_abs" "$link_dir")"
  ln -s "$rel" "$link_path"
  # Display the link path relative to the repo root (don't follow
  # the symlink we just created — realpath -s suppresses that).
  echo "   $(realpath -s --relative-to="$HERE" "$link_path") -> $rel"
}

echo ">> Wiring symlinks..."
link_relative "$HERE/common" \
              "$ESP_HOSTED_MCU_DIR/common"
link_relative "$HERE/coprocessor/main" \
              "$ESP_HOSTED_MCU_DIR/slave/main"
link_relative "$HERE/coprocessor/partitions.esp32c6.csv" \
              "$ESP_HOSTED_MCU_DIR/slave/partitions.esp32c6.csv"

cat <<EOF

Setup complete. To build and flash the coprocessor firmware:

  idf.py -C coprocessor set-target esp32c6
  idf.py -C coprocessor -p /dev/<serial-device> flash

EOF
