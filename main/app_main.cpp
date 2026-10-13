/*
 * Copyright 2026 https://github.com/ppetr/esp32s3-prometheus-exporter
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "esp_err.h"
#include "esp_log.h"

#include "net_eth.h"
#include "metrics_store.h"
#include "channel_registry.h"
#include "lua_pump.h"
#include "http_api.h"
#include "upload_auth.h"
#include "usb_host_bridge.h"
#include "i2c_poll_source.h"

static const char *TAG = "app_main";

// Sequential bring-up. usb_host_bridge/i2c_poll_source are still stubs;
// net_eth, channel_registry, http_api, and the lua_pump task (Core 1) are
// real, but usb_host_bridge/i2c_poll_source not registering any channels
// yet means the pump just idles - there's nothing for it to do until
// those are wired to channel_registry_add_channel().
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "esp32-usb-bridge starting");

    net_eth_init();
    metrics_store_init();
    channel_registry_init();
    // upload_auth_enabled() stays true even if this fails (it's a
    // build-time-only check, see upload_auth.c) - must not boot half-init.
    ESP_ERROR_CHECK(upload_auth_init());
    http_api_start();
    usb_host_bridge_init();
    i2c_poll_source_init();
    lua_pump_start();

    ESP_LOGI(TAG, "bring-up complete (usb_host_bridge/i2c_poll_source are stubs — see DESIGN.md)");
}
