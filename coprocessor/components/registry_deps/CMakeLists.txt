# Stub component: no source code, no public headers. Its sole purpose
# is to host an idf_component.yml that declares the coprocessor build's
# Component Registry dependencies (esp_ptp_rpc today), since the
# coprocessor's `main` is a symlink to upstream esp-hosted-mcu/slave/main
# and we can't add a manifest there.
#
# A developer working on esp_ptp_rpc itself can place a symlink at
# components/esp_ptp_rpc (gitignored) — IDF will use that local copy
# in preference to the managed_components/ install triggered here.
idf_component_register()
