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

#pragma once

#include "channel_source.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Real channel_source_t backed by periodic I2C register reads
// (DESIGN.md §9.1, poll-driven parser shape). Thin by design: moves bytes,
// does no parsing or scheduling.
esp_err_t i2c_poll_source_init(void);
esp_err_t i2c_poll_source_get_source(uint8_t device_addr, channel_source_t *out_source);

#ifdef __cplusplus
}
#endif
