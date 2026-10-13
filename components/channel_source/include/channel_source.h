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

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Narrow interface between the scheduler/Lua pump and a data-producing
// channel (USB CDC, I2C poll, or a fake used for off-hardware testing).
// See DESIGN.md §12.1.
typedef struct {
    esp_err_t (*open)(void *ctx);
    // Returns bytes read, 0 on timeout, <0 on error/disconnect.
    int (*read)(void *ctx, uint8_t *buf, size_t maxlen, uint32_t timeout_ms);
    bool (*is_connected)(void *ctx);
    void *ctx;
} channel_source_t;

#ifdef __cplusplus
}
#endif
