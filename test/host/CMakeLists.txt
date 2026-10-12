cmake_minimum_required(VERSION 3.16)

# Tier 1 host-native unit tests (DESIGN.md §12.2). Build for the ESP-IDF
# Linux target: idf.py --preview set-target linux build
#
# Only list components with no hardware-only dependencies (no "driver", "usb",
# "esp_eth", "esp_http_server", ...) here — those can't build for the Linux
# target. Add to this list as more logic-only pieces get tests (channel_source).
#
# http_api is NOT listed: its request-handling logic
# (http_api_handle_script_upload) is a one-line passthrough to
# channel_registry_load_script(), already fully exercised by
# test_channel_registry.c - pulling in the whole http_api component here
# would also pull in its PRIV_REQUIRES esp_http_server (and transitively
# lwip), unproven on the Linux target, for no additional test coverage.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/metrics_store"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/lua_sandbox"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/channel_registry")

# EXTRA_COMPONENT_DIRS otherwise forces a non-minimal build that expands
# requirements for every component under $IDF_PATH/components, which hits an
# upstream v6.1 bug (esp_driver_ana_cmpr REQUIRES esp_hal_ana_cmpr without a
# linux-target guard, while esp_hal_ana_cmpr itself returns early on linux).
# Restricting to "main" keeps the build to main's actual transitive REQUIRES.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Catches buffer overruns, use-after-free, signed overflow, and misaligned
# access on every test run, at zero cost to the embedded build (DESIGN.md
# §14) - relevant now that metrics_store does real heap allocation for
# histogram buckets.
idf_build_set_property(COMPILE_OPTIONS "-fsanitize=address,undefined" APPEND)
idf_build_set_property(LINK_OPTIONS "-fsanitize=address,undefined" APPEND)

# lmathlib.c (via lua_sandbox's espressif/lua dependency) needs libm
# (log10f et al.) on the Linux target; the esp32s3 build gets this via the
# Xtensa toolchain spec automatically, this one doesn't.
idf_build_set_property(LINK_OPTIONS "-lm" APPEND)

project(test_host)
