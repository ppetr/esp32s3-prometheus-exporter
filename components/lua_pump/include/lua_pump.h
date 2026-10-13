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
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fixed logical-timeout threshold for recovering a stalled channel
// (DESIGN.md §9.2) - a reasonable default, not a measured figure, same
// status as metrics_store's staleness threshold (§6.3 point 4).
#define LUA_PUMP_STALL_MS 60000

// Default instruction limit for every resume the pump triggers (DESIGN.md
// §9.2's CPU runaway guard) - matches the example in §9.2.
#define LUA_PUMP_INSTRUCTION_LIMIT 10000

// How long the task sleeps when a step found nothing to do, before
// checking again (DESIGN.md §9.1 describes a true "wait on data OR
// deadline" - this polling interval is a deliberate simplification; see
// DESIGN.md §9.1 for why it's an acceptable one at this project's scale).
#define LUA_PUMP_IDLE_SLEEP_MS 20

// Runs one scheduling pass (DESIGN.md §9.1): drains and resumes every
// channel with pending data (channel_registry_push_data()), then resumes
// the single most-overdue poll-driven channel if one is due
// (channel_registry_pick_due_channel()), then recovers any stalled
// channel (channel_registry_recover_stalled_channels(), using
// LUA_PUMP_STALL_MS). instruction_limit bounds every resume this
// triggers. Reports each resume's outcome via
// metrics_store_set_channel_status() (DESIGN.md §6.3).
//
// Returns true if any channel was resumed, false if there was nothing to
// do (the caller - lua_pump_start()'s task loop - should sleep before
// calling again). Kept separate from that task loop so it's unit-testable
// without a real FreeRTOS task or real delays (DESIGN.md §12.2).
bool lua_pump_step(uint32_t now_ticks, int instruction_limit);

// Starts the Lua pump FreeRTOS task (Core 1, medium priority - DESIGN.md
// §5): loops lua_pump_step(), sleeping LUA_PUMP_IDLE_SLEEP_MS when there's
// nothing to do, registering with the Task Watchdog on a best-effort basis
// (not fatal if the Task Watchdog isn't configured in this build).
esp_err_t lua_pump_start(void);

// Stops and deletes the pump task started by lua_pump_start(). Mainly for
// tests; production firmware has no normal reason to call this.
void lua_pump_stop(void);

#ifdef __cplusplus
}
#endif
