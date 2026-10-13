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
    "${CMAKE_CURRENT_LIST_DIR}/../../components/channel_registry"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/lua_pump"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/upload_auth")

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

# Workaround: this ESP-IDF version's mbedtls (TF-PSA-Crypto 4.x) has a
# circular static-library dependency between its own sub-libraries
# (library/libmbedtls.a <-> tf-psa-crypto/core/libtfpsacrypto.a) that GNU ld
# can't resolve in one left-to-right pass on this target, producing
# "undefined reference to mbedtls_ms_time"/"esp_mbedtls_mem_calloc" at link
# time. Wrap mbedtls's own propagated libraries in start/end-group so ld
# rescans them as needed.
idf_component_get_property(mbedtls_lib mbedtls COMPONENT_LIB)
get_property(mbedtls_link_libs TARGET ${mbedtls_lib} PROPERTY INTERFACE_LINK_LIBRARIES)
# mbedtls's own archive (esp_mem.c's esp_mbedtls_mem_calloc/free) is also
# part of the cycle - include it again inside the group; a repeated archive
# mention is harmless for ld.
set_property(TARGET ${mbedtls_lib} PROPERTY INTERFACE_LINK_LIBRARIES
    "-Wl,--start-group" "$<TARGET_FILE:${mbedtls_lib}>" ${mbedtls_link_libs} "-Wl,--end-group")
