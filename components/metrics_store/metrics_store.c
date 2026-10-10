#include "metrics_store.h"
#include "metrics_store_internal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define METRICS_STORE_MAX_ERROR_LEN 64
#define METRICS_STORE_STALENESS_TICKS pdMS_TO_TICKS(METRICS_STORE_STALENESS_MS)

typedef struct {
    char name[METRICS_STORE_MAX_NAME_LEN];
    float value;
    TickType_t last_update_tick;
    bool in_use;
} metric_entry_t;

typedef struct {
    char name[METRICS_STORE_MAX_NAME_LEN];
    bool in_use;
    bool online;
    char lua_error[METRICS_STORE_MAX_ERROR_LEN];
    metric_entry_t metrics[METRICS_STORE_MAX_METRICS_PER_CHANNEL];
} channel_entry_t;

static channel_entry_t s_channels[METRICS_STORE_MAX_CHANNELS];
static SemaphoreHandle_t s_lock;
static bool s_initialized;

esp_err_t metrics_store_init(void) {
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

void metrics_store_deinit(void) {
    if (!s_initialized) {
        return;
    }
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_initialized = false;
}

// Caller must hold s_lock.
static channel_entry_t *find_or_create_channel_locked(const char *channel) {
    channel_entry_t *free_slot = NULL;
    for (int i = 0; i < METRICS_STORE_MAX_CHANNELS; i++) {
        if (s_channels[i].in_use && strcmp(s_channels[i].name, channel) == 0) {
            return &s_channels[i];
        }
        if (!s_channels[i].in_use && free_slot == NULL) {
            free_slot = &s_channels[i];
        }
    }
    if (free_slot == NULL) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->name, sizeof(free_slot->name), "%s", channel);
    free_slot->in_use = true;
    free_slot->online = true;
    return free_slot;
}

// Caller must hold s_lock.
static metric_entry_t *find_or_create_metric_locked(channel_entry_t *ch, const char *metric_name) {
    metric_entry_t *free_slot = NULL;
    for (int i = 0; i < METRICS_STORE_MAX_METRICS_PER_CHANNEL; i++) {
        if (ch->metrics[i].in_use && strcmp(ch->metrics[i].name, metric_name) == 0) {
            return &ch->metrics[i];
        }
        if (!ch->metrics[i].in_use && free_slot == NULL) {
            free_slot = &ch->metrics[i];
        }
    }
    if (free_slot == NULL) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->name, sizeof(free_slot->name), "%s", metric_name);
    free_slot->in_use = true;
    return free_slot;
}

esp_err_t metrics_store_set(const char *channel, const char *metric_name, float value) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL || metric_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    channel_entry_t *ch = find_or_create_channel_locked(channel);
    if (ch == NULL) {
        err = ESP_ERR_NO_MEM;
    } else {
        metric_entry_t *m = find_or_create_metric_locked(ch, metric_name);
        if (m == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            m->value = value;
            m->last_update_tick = xTaskGetTickCount();
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t metrics_store_set_channel_status(const char *channel, bool online, const char *lua_error) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    channel_entry_t *ch = find_or_create_channel_locked(channel);
    if (ch == NULL) {
        err = ESP_ERR_NO_MEM;
    } else {
        ch->online = online;
        snprintf(ch->lua_error, sizeof(ch->lua_error), "%s", lua_error ? lua_error : "");
    }
    xSemaphoreGive(s_lock);
    return err;
}

// Escapes backslash, double-quote and newline for use inside a Prometheus
// label value, truncating (without overflowing dst) if it doesn't fit.
static void escape_label_value(const char *src, char *dst, size_t dst_len) {
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 2 < dst_len; i++) {
        char c = src[i];
        if (c == '\\' || c == '"') {
            dst[out++] = '\\';
            dst[out++] = c;
        } else if (c == '\n') {
            dst[out++] = '\\';
            dst[out++] = 'n';
        } else {
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
}

int metrics_store_format_at(char *buf, size_t buf_len, uint32_t now_ticks) {
    if (buf == NULL || buf_len == 0 || !s_initialized) {
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t offset = 0;
    bool overflow = false;
    for (int i = 0; i < METRICS_STORE_MAX_CHANNELS && !overflow; i++) {
        channel_entry_t *ch = &s_channels[i];
        if (!ch->in_use) {
            continue;
        }

        for (int j = 0; j < METRICS_STORE_MAX_METRICS_PER_CHANNEL; j++) {
            metric_entry_t *m = &ch->metrics[j];
            if (!m->in_use) {
                continue;
            }
            // Unsigned subtraction wraps correctly across tick-counter rollover.
            if ((TickType_t)(now_ticks - m->last_update_tick) > METRICS_STORE_STALENESS_TICKS) {
                continue;
            }
            int n = snprintf(buf + offset, buf_len - offset, "%s{channel=\"%s\"} %g\n", m->name,
                              ch->name, (double)m->value);
            if (n < 0 || (size_t)n >= buf_len - offset) {
                overflow = true;
                break;
            }
            offset += (size_t)n;
        }
        if (overflow) {
            break;
        }

        char escaped_error[METRICS_STORE_MAX_ERROR_LEN * 2];
        escape_label_value(ch->lua_error, escaped_error, sizeof(escaped_error));
        int n = (ch->lua_error[0] != '\0')
                    ? snprintf(buf + offset, buf_len - offset,
                               "usb_bridge_channel_info{channel=\"%s\",status=\"%s\",lua_error=\"%s\"} 1\n",
                               ch->name, ch->online ? "online" : "offline", escaped_error)
                    : snprintf(buf + offset, buf_len - offset,
                               "usb_bridge_channel_info{channel=\"%s\",status=\"%s\"} 1\n", ch->name,
                               ch->online ? "online" : "offline");
        if (n < 0 || (size_t)n >= buf_len - offset) {
            overflow = true;
            break;
        }
        offset += (size_t)n;
    }
    xSemaphoreGive(s_lock);

    return overflow ? -1 : (int)offset;
}

int metrics_store_format(char *buf, size_t buf_len) {
    return metrics_store_format_at(buf, buf_len, (uint32_t)xTaskGetTickCount());
}
