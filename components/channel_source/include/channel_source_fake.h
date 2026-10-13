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

#ifdef __cplusplus
extern "C" {
#endif

// Backs a channel_source_t by reading from a file (or FIFO/socket special
// file) instead of a real driver. Used by Tier 1 host tests and Tier 2 QEMU
// integration tests in place of usb_host_bridge / i2c_poll_source
// (DESIGN.md §12.1-§12.3).
esp_err_t channel_source_fake_create(const char *backing_path, channel_source_t *out_source);
void channel_source_fake_destroy(channel_source_t *source);

#ifdef __cplusplus
}
#endif
