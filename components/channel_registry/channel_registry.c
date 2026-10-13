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

#include "channel_registry.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lua_sandbox.h"

// "Never" for next_run_ticks purposes - comfortably below INT32_MAX after
// the (int32_t) delta cast used in pick_due_channel()/recover_stalled_
// channels(), so it stays unambiguously "far in the future" rather than
// risking wrapping negative.
#define CHANNEL_REGISTRY_NO_SCHEDULE_TICKS (UINT32_MAX / 4)

typedef struct {
    char name[CHANNEL_REGISTRY_MAX_NAME_LEN];
    bool in_use;
    lua_sandbox_t *sandbox;

    // Scheduling (DESIGN.md §9.1-§9.2). next_run_ticks/last_resume_tick are
    // written using the real clock (xTaskGetTickCount(), inside
    // channel_registry_resume()/_load_script()) but read against a
    // caller-supplied now_ticks in channel_registry_pick_due_channel()/
    // _recover_stalled_channels() - same split as metrics_store's
    // write-real-time/read-with-injectable-now (DESIGN.md §6.3), so those
    // two decision functions stay unit-testable without real delays.
    TickType_t next_run_ticks;
    TickType_t last_resume_tick;
    // Heap-allocated copy of the last successfully loaded script, or NULL
    // if none has been loaded yet - needed to recreate a stalled channel's
    // coroutine from scratch with the same script.
    char *last_source;

    // Pending bytes from a producer (future USB Host CDC task), awaiting
    // the next resume (DESIGN.md §9, §8).
    uint8_t pending_data[CHANNEL_REGISTRY_PENDING_DATA_BYTES];
    size_t pending_len;
} channel_entry_t;

static channel_entry_t s_channels[CHANNEL_REGISTRY_MAX_CHANNELS];
// One global lock, not a per-channel lock: resumes all come from the single
// Lua pump task (DESIGN.md §9.1), so they're already serialized against
// each other - the only real race this guards is an HTTP upload (Core 0)
// swapping a channel's script concurrently with the pump resuming it
// (Core 1). Simplest option that's still correct, matching the
// metrics_store precedent (DESIGN.md §6.2).
static SemaphoreHandle_t s_lock;
static bool s_initialized;

esp_err_t channel_registry_init(void) {
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_channels, 0, sizeof(s_channels));
    s_initialized = true;
    return ESP_OK;
}

void channel_registry_deinit(void) {
    if (!s_initialized) {
        return;
    }
    for (int i = 0; i < CHANNEL_REGISTRY_MAX_CHANNELS; i++) {
        if (s_channels[i].in_use) {
            lua_sandbox_destroy(s_channels[i].sandbox);
            free(s_channels[i].last_source);
        }
    }
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_initialized = false;
}

// Caller must hold s_lock.
static channel_entry_t *find_channel_locked(const char *channel) {
    for (int i = 0; i < CHANNEL_REGISTRY_MAX_CHANNELS; i++) {
        if (s_channels[i].in_use && strcmp(s_channels[i].name, channel) == 0) {
            return &s_channels[i];
        }
    }
    return NULL;
}

esp_err_t channel_registry_add_channel(const char *channel, size_t max_memory_bytes) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (find_channel_locked(channel) != NULL) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        channel_entry_t *free_slot = NULL;
        for (int i = 0; i < CHANNEL_REGISTRY_MAX_CHANNELS; i++) {
            if (!s_channels[i].in_use) {
                free_slot = &s_channels[i];
                break;
            }
        }
        if (free_slot == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            lua_sandbox_t *sandbox = NULL;
            err = lua_sandbox_create(channel, max_memory_bytes, &sandbox);
            if (err == ESP_OK) {
                snprintf(free_slot->name, sizeof(free_slot->name), "%s", channel);
                free_slot->sandbox = sandbox;
                free_slot->in_use = true;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t channel_registry_remove_channel(const char *channel) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    channel_entry_t *entry = find_channel_locked(channel);
    if (entry == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        lua_sandbox_destroy(entry->sandbox);
        free(entry->last_source);
        memset(entry, 0, sizeof(*entry));
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t channel_registry_load_script(const char *channel, const char *source, char *err_buf,
                                        size_t err_buf_len) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL || source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Dry-run against a throwaway state (DESIGN.md §10 point 1) - no lock
    // needed, this never touches any channel's live sandbox.
    esp_err_t err = lua_sandbox_check_syntax(source, err_buf, err_buf_len);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    channel_entry_t *entry = find_channel_locked(channel);
    if (entry == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        // The atomic swap itself: lua_sandbox_load() tears down the old
        // coroutine and installs the new one in one call.
        err = lua_sandbox_load(entry->sandbox, source);
        if (err == ESP_OK) {
            size_t source_len = strlen(source) + 1;
            char *copy = malloc(source_len);
            if (copy == NULL) {
                err = ESP_ERR_NO_MEM;
            } else {
                memcpy(copy, source, source_len);
                free(entry->last_source);
                entry->last_source = copy;
                // A freshly (re)loaded script hasn't had a chance to run
                // yet - due immediately, and not stalled until it's had
                // that chance.
                entry->last_resume_tick = xTaskGetTickCount();
                entry->next_run_ticks = entry->last_resume_tick;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t channel_registry_validate_script(const char *channel, const char *source, char *err_buf,
                                            size_t err_buf_len) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL || source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lua_sandbox_check_syntax(source, err_buf, err_buf_len);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = (find_channel_locked(channel) == NULL) ? ESP_ERR_NOT_FOUND : ESP_OK;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t channel_registry_resume(const char *channel, const uint8_t *data, size_t len,
                                   int instruction_limit, uint32_t *yield_ms_out, char *err_buf,
                                   size_t err_buf_len) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err;
    channel_entry_t *entry = find_channel_locked(channel);
    if (entry == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        err = lua_sandbox_resume(entry->sandbox, data, len, instruction_limit, yield_ms_out, err_buf, err_buf_len);
        if (err == ESP_OK) {
            entry->last_resume_tick = xTaskGetTickCount();
            if (*yield_ms_out > 0) {
                entry->next_run_ticks = entry->last_resume_tick + pdMS_TO_TICKS(*yield_ms_out);
            } else {
                // No explicit schedule requested (a stream-driven
                // coroutine.yield() with no value, or the coroutine ran to
                // completion) - don't keep auto-polling this channel; only
                // new pending data (channel_registry_push_data()) or a
                // future numeric yield should wake it again (DESIGN.md
                // §9.1). Without this, a stream parser paused mid-message
                // would get spuriously re-resumed with no data on the next
                // scheduler pass.
                entry->next_run_ticks = entry->last_resume_tick + CHANNEL_REGISTRY_NO_SCHEDULE_TICKS;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t channel_registry_pick_due_channel(uint32_t now_ticks, char *out_channel, size_t out_channel_len,
                                             uint32_t *out_wait_ticks) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_channel == NULL || out_channel_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    channel_entry_t *best = NULL;
    int32_t best_delta = 0;  // next_run_ticks - now_ticks for `best`; <= 0 means due.
    for (int i = 0; i < CHANNEL_REGISTRY_MAX_CHANNELS; i++) {
        if (!s_channels[i].in_use) {
            continue;
        }
        int32_t delta = (int32_t)(s_channels[i].next_run_ticks - now_ticks);
        if (best == NULL || delta < best_delta) {
            best = &s_channels[i];
            best_delta = delta;
        }
    }

    esp_err_t err;
    if (best == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else if (best_delta <= 0) {
        snprintf(out_channel, out_channel_len, "%s", best->name);
        err = ESP_OK;
    } else {
        if (out_wait_ticks != NULL) {
            *out_wait_ticks = (uint32_t)best_delta;
        }
        err = ESP_ERR_NOT_FOUND;
    }
    xSemaphoreGive(s_lock);
    return err;
}

int channel_registry_recover_stalled_channels(uint32_t now_ticks, uint32_t stall_ticks) {
    if (!s_initialized) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int recovered = 0;
    for (int i = 0; i < CHANNEL_REGISTRY_MAX_CHANNELS; i++) {
        channel_entry_t *entry = &s_channels[i];
        if (!entry->in_use || entry->last_source == NULL) {
            continue;
        }
        int32_t idle = (int32_t)(now_ticks - entry->last_resume_tick);
        if (idle <= (int32_t)stall_ticks) {
            continue;
        }
        // Best-effort: if reloading fails, leave the stalled coroutine as
        // it is rather than destroying it with nothing to replace it.
        if (lua_sandbox_load(entry->sandbox, entry->last_source) == ESP_OK) {
            entry->last_resume_tick = now_ticks;
            entry->next_run_ticks = now_ticks;
            recovered++;
        }
    }
    xSemaphoreGive(s_lock);
    return recovered;
}

esp_err_t channel_registry_push_data(const char *channel, const uint8_t *data, size_t len) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err;
    channel_entry_t *entry = find_channel_locked(channel);
    if (entry == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else if (entry->pending_len + len > sizeof(entry->pending_data)) {
        err = ESP_ERR_NO_MEM;
    } else {
        memcpy(entry->pending_data + entry->pending_len, data, len);
        entry->pending_len += len;
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t channel_registry_get_memory_usage(const char *channel, size_t *out_used_bytes, size_t *out_limit_bytes) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL || out_used_bytes == NULL || out_limit_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err;
    channel_entry_t *entry = find_channel_locked(channel);
    if (entry == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        *out_used_bytes = lua_sandbox_memory_used(entry->sandbox);
        *out_limit_bytes = lua_sandbox_memory_limit(entry->sandbox);
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

int channel_registry_pick_channel_with_data(char *out_channel, size_t out_channel_len, uint8_t *buf, size_t maxlen) {
    if (!s_initialized || out_channel == NULL || out_channel_len == 0 || buf == NULL) {
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int drained = 0;
    for (int i = 0; i < CHANNEL_REGISTRY_MAX_CHANNELS; i++) {
        channel_entry_t *entry = &s_channels[i];
        if (!entry->in_use || entry->pending_len == 0) {
            continue;
        }
        drained = (int)(entry->pending_len < maxlen ? entry->pending_len : maxlen);
        memcpy(buf, entry->pending_data, (size_t)drained);
        // Drop anything beyond maxlen rather than keeping a partial
        // remainder - maxlen is expected to comfortably cover
        // CHANNEL_REGISTRY_PENDING_DATA_BYTES in practice.
        entry->pending_len = 0;
        snprintf(out_channel, out_channel_len, "%s", entry->name);
        break;
    }
    xSemaphoreGive(s_lock);
    return drained;
}
