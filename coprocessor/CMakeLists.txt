# Build root for the ESP-AVB-Bridge coprocessor firmware.
#
# Mirrors upstream esp-hosted-mcu/slave/CMakeLists.txt with two additions:
# pulls in the esp_ptp_rpc component (symlinked into components/) and
# chains this directory's sdkconfig.defaults on top of upstream's.
# Upstream's slave/main is mounted unchanged via the `main` symlink.
# partitions.esp32c6.csv is also a symlink to upstream. No upstream
# source is copied here.
#
# Path layout: this CMakeLists.txt lives at .../coprocessor/CMakeLists.txt.
# Upstream's main/CMakeLists.txt has a hardcoded "../../common" relative
# path fallback, so a `common` symlink at ESP-AVB-Bridge/common
# (one level above this project root) satisfies it.

cmake_minimum_required(VERSION 3.16)

# Match upstream's link-time wrap of esp_wifi_init.
add_compile_options(-Wl,--wrap=esp_wifi_init)

# Mirror upstream's version-from-header trick so PROJECT_VER is set
# from esp_hosted_coprocessor_fw_ver.h before project() runs.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/main/esp_hosted_coprocessor_fw_ver.h" HEADER_CONTENTS)
set(PROJECT_VERSION_REGEX "#define[ \t]+PROJECT_VERSION_MAJOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_MINOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_PATCH_1[ \t]+([0-9]+)")
string(REGEX MATCH "${PROJECT_VERSION_REGEX}" PROJECT_VERSION_MATCH "${HEADER_CONTENTS}")
if(PROJECT_VERSION_MATCH)
    set(PROJECT_VER "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    message(STATUS "Setting PROJECT_VER to: ${PROJECT_VER}")
endif()

# Resolve the upstream esp-hosted-mcu/slave/ dir via the `main`
# symlink so we don't hardcode an absolute path. file(REAL_PATH)
# follows symlinks before applying ..; get_filename_component(... REALPATH)
# does lexical .. resolution only.
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/main" UPSTREAM_MAIN)
get_filename_component(UPSTREAM_PROJECT "${UPSTREAM_MAIN}" DIRECTORY)

# Chain SDKCONFIG_DEFAULTS: upstream coprocessor defaults first, then
# this directory's sdkconfig.defaults as overrides.
set(SDKCONFIG_DEFAULTS
    "${UPSTREAM_PROJECT}/sdkconfig.defaults"
    "${UPSTREAM_PROJECT}/sdkconfig.defaults.esp32c6"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
list(APPEND EXCLUDE_COMPONENTS lwip esp_netif)

# Upstream's COMPONENTS allowlist + our esp_ptp_rpc + registry_deps.
# registry_deps is the stub component that hosts the idf_component.yml
# declaring scrambletools/esp_ptp_rpc as a managed dependency, since
# upstream slave/main (our `main`) can't be modified. esp_ptp_rpc is
# also listed so it survives the allowlist filter regardless of
# whether it resolves to managed_components/ (registry) or
# components/esp_ptp_rpc/ (developer override symlink).
# app_update supplies esp_ota_ops.h for upstream's slave_control.c. It used
# to arrive transitively; IDF 6.x tightened component resolution and it must
# now be listed explicitly, so this line diverges from upstream's allowlist.
set(COMPONENTS esp_timer esptool_py bootloader main nvs_flash esp_rom esp_wifi protocomm driver mqtt wpa_supplicant wifi_provisioning esp_driver_sdio esp_pm openthread app_update esp_ptp_rpc registry_deps)

project(network_adapter)
idf_build_set_property(COMPILE_OPTIONS "-fdiagnostics-color=always" APPEND)

# Mirror upstream's `common` directory resolution. PROJECT_DIR/../common
# resolves through ESP-AVB-Bridge/common (a symlink to upstream's
# common/) so upstream's main/CMakeLists.txt fallback "../../common"
# also resolves correctly relative to main/.
if(EXISTS "${PROJECT_DIR}/../common")
    set(common_dir "${PROJECT_DIR}/../common")
elseif(EXISTS "${PROJECT_DIR}/main/common")
    set(common_dir "${PROJECT_DIR}/main/common")
else()
    message(FATAL_ERROR "Could not find 'common' directory at '${PROJECT_DIR}/../common' or '${PROJECT_DIR}/main/common'")
endif()
message(STATUS "common: ${common_dir}")

message(*************************************************************************************)
message("                    Building ESP-Hosted-MCU FW :: ${PROJECT_VER} ")
message(*************************************************************************************)
