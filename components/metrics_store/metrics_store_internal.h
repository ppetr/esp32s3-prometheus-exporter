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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed staleness threshold (DESIGN.md §6.3 point 4: a simple fixed default
// is acceptable for v1, not a user-facing config knob).
#define METRICS_STORE_STALENESS_MS 60000

// metrics_store_format() with an injectable tick count, so the
// staleness-omission logic above can be unit tested without waiting in real
// time. Not part of the public API - see metrics_store.h.
int metrics_store_format_at(char *buf, size_t buf_len, uint32_t now_ticks);

#ifdef __cplusplus
}
#endif
