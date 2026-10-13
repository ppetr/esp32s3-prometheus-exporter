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

#include "lua_pump.h"

#include "channel_registry.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "metrics_store.h"

static TaskHandle_t s_task;

// Reports one resume's outcome (DESIGN.md §6.3): clears any previous error
// on success, records the message on a runtime error. Silent on
// ESP_ERR_NOT_FOUND et al. - the channel vanished between being picked and
// resumed (e.g. a concurrent remove), nothing to report.
static void report_result(const char *channel, esp_err_t err, const char *err_buf) {
    if (err == ESP_OK) {
        metrics_store_set_channel_status(channel, true, NULL);
    } else if (err == ESP_FAIL) {
        metrics_store_set_channel_status(channel, true, err_buf);
    }

    // Lua-side memory usage (DESIGN.md §7), refreshed on every resume
    // regardless of its outcome - the sandbox still holds whatever it
    // allocated even after a runtime error.
    size_t used_bytes, limit_bytes;
    if (channel_registry_get_memory_usage(channel, &used_bytes, &limit_bytes) == ESP_OK) {
        metrics_store_set(channel, "lua_memory_used_bytes", (float)used_bytes);
        metrics_store_set(channel, "lua_memory_limit_bytes", (float)limit_bytes);
    }
}

bool lua_pump_step(uint32_t now_ticks, int instruction_limit) {
    bool did_work = false;
    char channel[CHANNEL_REGISTRY_MAX_NAME_LEN];
    uint8_t buf[CHANNEL_REGISTRY_PENDING_DATA_BYTES];
    char err_buf[128];
    uint32_t yield_ms;
    esp_err_t err;

    // Drain and resume every channel with pending data. Bounded by the
    // channel count so a never-empty buffer can't loop forever.
    for (int i = 0; i < CHANNEL_REGISTRY_MAX_CHANNELS; i++) {
        int drained = channel_registry_pick_channel_with_data(channel, sizeof(channel), buf, sizeof(buf));
        if (drained <= 0) {
            break;
        }
        err_buf[0] = '\0';
        yield_ms = 0;
        err = channel_registry_resume(channel, buf, (size_t)drained, instruction_limit, &yield_ms, err_buf,
                                       sizeof(err_buf));
        report_result(channel, err, err_buf);
        did_work = true;
    }

    // One poll-driven channel per step, if due.
    uint32_t wait_ticks;
    if (channel_registry_pick_due_channel(now_ticks, channel, sizeof(channel), &wait_ticks) == ESP_OK) {
        err_buf[0] = '\0';
        yield_ms = 0;
        err = channel_registry_resume(channel, NULL, 0, instruction_limit, &yield_ms, err_buf, sizeof(err_buf));
        report_result(channel, err, err_buf);
        did_work = true;
    }

    channel_registry_recover_stalled_channels(now_ticks, pdMS_TO_TICKS(LUA_PUMP_STALL_MS));

    return did_work;
}

static void lua_pump_task(void *arg) {
    (void)arg;
#ifdef CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_add(NULL);  // best-effort - not fatal if adding ever fails
#endif

    while (true) {
        bool did_work = lua_pump_step((uint32_t)xTaskGetTickCount(), LUA_PUMP_INSTRUCTION_LIMIT);
#ifdef CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif
        // Always yield at least one tick, even right after doing work: a
        // channel polling faster than the tick period (e.g.
        // coroutine.yield(5) under a 10ms tick) would otherwise look
        // immediately due again forever, spinning this task without ever
        // blocking and starving everything else at this priority.
        vTaskDelay(did_work ? 1 : pdMS_TO_TICKS(LUA_PUMP_IDLE_SLEEP_MS));
    }
}

esp_err_t lua_pump_start(void) {
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // Core 1 on real (dual-core) hardware per DESIGN.md §5; the Linux
    // target's FreeRTOS port only simulates one core, where core 1
    // doesn't exist.
    BaseType_t core = (configNUMBER_OF_CORES > 1) ? 1 : 0;
    BaseType_t ok =
        xTaskCreatePinnedToCore(lua_pump_task, "lua_pump", 4096, NULL, tskIDLE_PRIORITY + 10, &s_task, core);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

void lua_pump_stop(void) {
    if (s_task == NULL) {
        return;
    }
    vTaskDelete(s_task);
    s_task = NULL;
}
