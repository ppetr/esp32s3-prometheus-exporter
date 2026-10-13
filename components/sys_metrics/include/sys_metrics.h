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

#ifdef __cplusplus
extern "C" {
#endif

// Reads live heap/PSRAM/CPU/stack usage and pushes it into metrics_store
// (DESIGN.md §7). Call once per /metrics scrape, right before
// metrics_store_format() - CPU usage needs a previous sample to compute a
// delta, so the first call after boot only establishes a baseline and
// won't appear in the exposition yet.
void sys_metrics_refresh(void);

#ifdef __cplusplus
}
#endif
