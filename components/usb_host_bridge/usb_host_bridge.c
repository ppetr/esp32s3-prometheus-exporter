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

#include "usb_host_bridge.h"

#include "esp_log.h"

static const char *TAG = "usb_host_bridge";

// Not yet implemented: USB Host install + CDC-ACM driver bring-up
// (ESP-IDF usb_host_cdc_acm component), per DESIGN.md §8-§9.1.
esp_err_t usb_host_bridge_init(void) {
    ESP_LOGW(TAG, "usb_host_bridge_init: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t usb_host_bridge_get_source(channel_source_t *out_source) {
    (void)out_source;
    return ESP_ERR_NOT_SUPPORTED;
}
