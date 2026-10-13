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

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up Ethernet (W5500 over SPI on real hardware, DESIGN.md §2) and
// attaches it to LwIP, then starts best-effort SNTP (DESIGN.md §6.4). The
// Tier 2 QEMU build variant swaps the W5500-specific bring-up for QEMU's
// emulated netif here (DESIGN.md §12.3, §13).
esp_err_t net_eth_init(void);

// True once SNTP has completed at least one sync; gates whether the HTTP
// exporter appends a Unix timestamp (DESIGN.md §6.4).
bool net_eth_time_synchronized(void);

#ifdef __cplusplus
}
#endif
