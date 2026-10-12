#include "channel_registry.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lua_sandbox.h"

typedef struct {
    char name[CHANNEL_REGISTRY_MAX_NAME_LEN];
    bool in_use;
    lua_sandbox_t *sandbox;
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
            err = lua_sandbox_create(max_memory_bytes, &sandbox);
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
    }
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
    }
    xSemaphoreGive(s_lock);
    return err;
}
