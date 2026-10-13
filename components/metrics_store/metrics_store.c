#include "metrics_store.h"
#include "metrics_store_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define METRICS_STORE_MAX_ERROR_LEN 64
#define METRICS_STORE_STALENESS_TICKS pdMS_TO_TICKS(METRICS_STORE_STALENESS_MS)

typedef enum {
    METRIC_KIND_GAUGE = 0,
    METRIC_KIND_HISTOGRAM = 1,
    METRIC_KIND_COUNTER = 2,
} metric_kind_t;

typedef struct {
    char name[METRICS_STORE_MAX_NAME_LEN];
    bool in_use;
    metric_kind_t kind;
    TickType_t last_update_tick;

    // Valid when kind == METRIC_KIND_GAUGE.
    float value;

    // Valid when kind == METRIC_KIND_COUNTER. Cumulative; only ever
    // increases (metrics_store_counter_add() rejects negative amounts).
    double counter_value;

    // Valid when kind == METRIC_KIND_HISTOGRAM. bucket_upper_bounds/
    // bucket_counts are heap-allocated once in metrics_store_histogram_init(),
    // sized exactly to bucket_count - never reallocated on the observe hot
    // path. bucket_counts[i] is the (non-cumulative) count of observations in
    // (bucket_upper_bounds[i-1], bucket_upper_bounds[i]]; cumulative sums are
    // computed when formatting.
    size_t bucket_count;
    float *bucket_upper_bounds;
    uint32_t *bucket_counts;
    uint32_t total_count;
    double sum;
} metric_entry_t;

typedef struct {
    char name[METRICS_STORE_MAX_NAME_LEN];
    bool in_use;
    bool online;
    char lua_error[METRICS_STORE_MAX_ERROR_LEN];
    metric_entry_t metrics[METRICS_STORE_MAX_METRICS_PER_CHANNEL];
} channel_entry_t;

// One task's stack headroom (DESIGN.md §7), as last reported by
// sys_metrics_refresh(). Not channel-scoped - a "task" label, not
// "channel".
#define METRICS_STORE_MAX_TASKS 32

typedef struct {
    char name[METRICS_STORE_MAX_NAME_LEN];
    uint32_t stack_free_bytes;
} task_stack_entry_t;

static channel_entry_t s_channels[METRICS_STORE_MAX_CHANNELS];
static SemaphoreHandle_t s_lock;
static bool s_initialized;

// Process-wide diagnostics (DESIGN.md §7), pushed by sys_metrics/net_eth
// and read back by metrics_store_format_at() - same "write now, read
// later" split as channel metrics, just without a channel label since
// these aren't scoped to any one channel.
static bool s_have_memory_info;
static size_t s_heap_free_bytes, s_heap_total_bytes, s_psram_free_bytes, s_psram_total_bytes;

static bool s_have_cpu_usage;
static float s_cpu0_usage_percent, s_cpu1_usage_percent;

static bool s_have_eth_link;
static bool s_eth_link_up;
static int s_eth_link_speed_mbps;
static bool s_eth_full_duplex;

static task_stack_entry_t s_task_stacks[METRICS_STORE_MAX_TASKS];
static int s_task_stack_count;

esp_err_t metrics_store_init(void) {
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_channels, 0, sizeof(s_channels));
    s_have_memory_info = false;
    s_have_cpu_usage = false;
    s_have_eth_link = false;
    s_task_stack_count = 0;
    s_initialized = true;
    return ESP_OK;
}

void metrics_store_deinit(void) {
    if (!s_initialized) {
        return;
    }
    for (int i = 0; i < METRICS_STORE_MAX_CHANNELS; i++) {
        for (int j = 0; j < METRICS_STORE_MAX_METRICS_PER_CHANNEL; j++) {
            free(s_channels[i].metrics[j].bucket_upper_bounds);
            free(s_channels[i].metrics[j].bucket_counts);
        }
    }
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_initialized = false;
}

// Caller must hold s_lock.
static channel_entry_t *find_channel_locked(const char *channel) {
    for (int i = 0; i < METRICS_STORE_MAX_CHANNELS; i++) {
        if (s_channels[i].in_use && strcmp(s_channels[i].name, channel) == 0) {
            return &s_channels[i];
        }
    }
    return NULL;
}

// Caller must hold s_lock.
static channel_entry_t *find_or_create_channel_locked(const char *channel) {
    channel_entry_t *existing = find_channel_locked(channel);
    if (existing != NULL) {
        return existing;
    }
    channel_entry_t *free_slot = NULL;
    for (int i = 0; i < METRICS_STORE_MAX_CHANNELS; i++) {
        if (!s_channels[i].in_use) {
            free_slot = &s_channels[i];
            break;
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
static metric_entry_t *find_metric_locked(channel_entry_t *ch, const char *metric_name) {
    for (int i = 0; i < METRICS_STORE_MAX_METRICS_PER_CHANNEL; i++) {
        if (ch->metrics[i].in_use && strcmp(ch->metrics[i].name, metric_name) == 0) {
            return &ch->metrics[i];
        }
    }
    return NULL;
}

// Caller must hold s_lock. *out_created tells the caller whether this slot
// was just allocated (so it must still set `kind`) or already existed.
static metric_entry_t *find_or_create_metric_locked(channel_entry_t *ch, const char *metric_name,
                                                      bool *out_created) {
    metric_entry_t *existing = find_metric_locked(ch, metric_name);
    if (existing != NULL) {
        *out_created = false;
        return existing;
    }
    metric_entry_t *free_slot = NULL;
    for (int i = 0; i < METRICS_STORE_MAX_METRICS_PER_CHANNEL; i++) {
        if (!ch->metrics[i].in_use) {
            free_slot = &ch->metrics[i];
            break;
        }
    }
    if (free_slot == NULL) {
        return NULL;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    snprintf(free_slot->name, sizeof(free_slot->name), "%s", metric_name);
    free_slot->in_use = true;
    *out_created = true;
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
        bool created;
        metric_entry_t *m = find_or_create_metric_locked(ch, metric_name, &created);
        if (m == NULL) {
            err = ESP_ERR_NO_MEM;
        } else if (!created && m->kind != METRIC_KIND_GAUGE) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            m->kind = METRIC_KIND_GAUGE;
            m->value = value;
            m->last_update_tick = xTaskGetTickCount();
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t metrics_store_counter_add(const char *channel, const char *metric_name, double amount) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL || metric_name == NULL || amount < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    channel_entry_t *ch = find_or_create_channel_locked(channel);
    if (ch == NULL) {
        err = ESP_ERR_NO_MEM;
    } else {
        bool created;
        metric_entry_t *m = find_or_create_metric_locked(ch, metric_name, &created);
        if (m == NULL) {
            err = ESP_ERR_NO_MEM;
        } else if (!created && m->kind != METRIC_KIND_COUNTER) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            if (created) {
                m->kind = METRIC_KIND_COUNTER;
                m->counter_value = 0.0;
            }
            m->counter_value += amount;
            m->last_update_tick = xTaskGetTickCount();
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t metrics_store_histogram_init(const char *channel, const char *metric_name,
                                        const float *bucket_upper_bounds, size_t bucket_count) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL || metric_name == NULL || bucket_upper_bounds == NULL || bucket_count == 0 ||
        bucket_count > METRICS_STORE_MAX_HISTOGRAM_BUCKETS) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 1; i < bucket_count; i++) {
        if (bucket_upper_bounds[i] <= bucket_upper_bounds[i - 1]) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    channel_entry_t *ch = find_or_create_channel_locked(channel);
    if (ch == NULL) {
        err = ESP_ERR_NO_MEM;
    } else {
        bool created;
        metric_entry_t *m = find_or_create_metric_locked(ch, metric_name, &created);
        if (m == NULL) {
            err = ESP_ERR_NO_MEM;
        } else if (!created && m->kind != METRIC_KIND_HISTOGRAM) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            float *new_bounds = malloc(bucket_count * sizeof(float));
            uint32_t *new_counts = calloc(bucket_count, sizeof(uint32_t));
            if (new_bounds == NULL || new_counts == NULL) {
                free(new_bounds);
                free(new_counts);
                if (created) {
                    m->in_use = false;  // don't leak a half-initialized slot
                }
                err = ESP_ERR_NO_MEM;
            } else {
                free(m->bucket_upper_bounds);
                free(m->bucket_counts);
                memcpy(new_bounds, bucket_upper_bounds, bucket_count * sizeof(float));
                m->bucket_upper_bounds = new_bounds;
                m->bucket_counts = new_counts;
                m->bucket_count = bucket_count;
                m->kind = METRIC_KIND_HISTOGRAM;
                m->total_count = 0;
                m->sum = 0.0;
                m->last_update_tick = xTaskGetTickCount();
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t metrics_store_histogram_observe(const char *channel, const char *metric_name, float value) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel == NULL || metric_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    channel_entry_t *ch = find_channel_locked(channel);
    metric_entry_t *m = (ch != NULL) ? find_metric_locked(ch, metric_name) : NULL;
    if (m == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else if (m->kind != METRIC_KIND_HISTOGRAM) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        size_t idx = m->bucket_count;  // bucket_count == "falls only in +Inf"
        for (size_t i = 0; i < m->bucket_count; i++) {
            if (value <= m->bucket_upper_bounds[i]) {
                idx = i;
                break;
            }
        }
        if (idx < m->bucket_count) {
            m->bucket_counts[idx]++;
        }
        m->total_count++;
        m->sum += (double)value;
        m->last_update_tick = xTaskGetTickCount();
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

esp_err_t metrics_store_set_memory_info(size_t heap_free_bytes, size_t heap_total_bytes, size_t psram_free_bytes,
                                         size_t psram_total_bytes) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_heap_free_bytes = heap_free_bytes;
    s_heap_total_bytes = heap_total_bytes;
    s_psram_free_bytes = psram_free_bytes;
    s_psram_total_bytes = psram_total_bytes;
    s_have_memory_info = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t metrics_store_set_cpu_usage(float core0_percent, float core1_percent) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cpu0_usage_percent = core0_percent;
    s_cpu1_usage_percent = core1_percent;
    s_have_cpu_usage = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t metrics_store_set_eth_link(bool up, int speed_mbps, bool full_duplex) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_eth_link_up = up;
    s_eth_link_speed_mbps = speed_mbps;
    s_eth_full_duplex = full_duplex;
    s_have_eth_link = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t metrics_store_set_task_stacks(const char *const *names, const uint32_t *stack_free_bytes, size_t count) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (names == NULL || stack_free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > METRICS_STORE_MAX_TASKS) {
        count = METRICS_STORE_MAX_TASKS;  // diagnostic only - truncate rather than fail
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < count; i++) {
        snprintf(s_task_stacks[i].name, sizeof(s_task_stacks[i].name), "%s", names[i]);
        s_task_stacks[i].stack_free_bytes = stack_free_bytes[i];
    }
    s_task_stack_count = (int)count;
    xSemaphoreGive(s_lock);
    return ESP_OK;
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

// Appends one snprintf-formatted chunk at *offset, flagging overflow instead
// of ever writing past buf_len. Caller must hold s_lock.
static bool append(char *buf, size_t buf_len, size_t *offset, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *offset, buf_len - *offset, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= buf_len - *offset) {
        return false;
    }
    *offset += (size_t)n;
    return true;
}

static bool format_gauge(char *buf, size_t buf_len, size_t *offset, const metric_entry_t *m,
                          const channel_entry_t *ch) {
    return append(buf, buf_len, offset, "%s{channel=\"%s\"} %g\n", m->name, ch->name, (double)m->value);
}

static bool format_counter(char *buf, size_t buf_len, size_t *offset, const metric_entry_t *m,
                            const channel_entry_t *ch) {
    return append(buf, buf_len, offset, "%s{channel=\"%s\"} %g\n", m->name, ch->name, m->counter_value);
}

static bool format_histogram(char *buf, size_t buf_len, size_t *offset, const metric_entry_t *m,
                              const channel_entry_t *ch) {
    uint32_t cumulative = 0;
    for (size_t i = 0; i < m->bucket_count; i++) {
        cumulative += m->bucket_counts[i];
        if (!append(buf, buf_len, offset, "%s_bucket{channel=\"%s\",le=\"%g\"} %lu\n", m->name, ch->name,
                    (double)m->bucket_upper_bounds[i], (unsigned long)cumulative)) {
            return false;
        }
    }
    if (!append(buf, buf_len, offset, "%s_bucket{channel=\"%s\",le=\"+Inf\"} %lu\n", m->name, ch->name,
                (unsigned long)m->total_count)) {
        return false;
    }
    if (!append(buf, buf_len, offset, "%s_sum{channel=\"%s\"} %g\n", m->name, ch->name, m->sum)) {
        return false;
    }
    return append(buf, buf_len, offset, "%s_count{channel=\"%s\"} %lu\n", m->name, ch->name,
                  (unsigned long)m->total_count);
}

int metrics_store_format_at(char *buf, size_t buf_len, uint32_t now_ticks) {
    if (buf == NULL || buf_len == 0 || !s_initialized) {
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t offset = 0;
    // Process-level, not channel-scoped - no "channel" label (DESIGN.md
    // §7's diagnostics intent). now_ticks is already ticks since the
    // scheduler started, i.e. since boot, so no separate epoch is needed.
    // Float, not integer seconds: the name says "_seconds" (Prometheus
    // convention: metric name encodes the unit), so sub-second resolution
    // belongs in the value, not a renamed/rescaled metric. Fixed-point
    // (not %g): %g would drop to scientific notation - and lose the
    // fractional part entirely - once uptime exceeds 6 significant
    // digits (~11.5 days), which defeats the point of millisecond
    // resolution for exactly the long-running case uptime is for.
    bool overflow = !append(buf, buf_len, &offset, "esp32_usb_bridge_uptime_seconds %.3f\n",
                             (double)now_ticks * portTICK_PERIOD_MS / 1000.0);

    if (!overflow && s_have_memory_info) {
        overflow = !append(buf, buf_len, &offset,
                            "esp32_usb_bridge_heap_free_bytes %zu\n"
                            "esp32_usb_bridge_heap_total_bytes %zu\n"
                            "esp32_usb_bridge_psram_free_bytes %zu\n"
                            "esp32_usb_bridge_psram_total_bytes %zu\n",
                            s_heap_free_bytes, s_heap_total_bytes, s_psram_free_bytes, s_psram_total_bytes);
    }

    if (!overflow && s_have_cpu_usage) {
        overflow = !append(buf, buf_len, &offset,
                            "esp32_usb_bridge_cpu_usage_percent{core=\"0\"} %.1f\n"
                            "esp32_usb_bridge_cpu_usage_percent{core=\"1\"} %.1f\n",
                            (double)s_cpu0_usage_percent, (double)s_cpu1_usage_percent);
    }

    if (!overflow && s_have_eth_link) {
        if (s_eth_link_up) {
            overflow = !append(buf, buf_len, &offset,
                                "esp32_usb_bridge_eth_link_up 1\n"
                                "esp32_usb_bridge_eth_link_speed_mbps %d\n"
                                "esp32_usb_bridge_eth_full_duplex %d\n",
                                s_eth_link_speed_mbps, s_eth_full_duplex ? 1 : 0);
        } else {
            overflow = !append(buf, buf_len, &offset, "esp32_usb_bridge_eth_link_up 0\n");
        }
    }

    for (int i = 0; i < s_task_stack_count && !overflow; i++) {
        overflow = !append(buf, buf_len, &offset, "esp32_usb_bridge_task_stack_free_bytes{task=\"%s\"} %lu\n",
                            s_task_stacks[i].name, (unsigned long)s_task_stacks[i].stack_free_bytes);
    }

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
            bool ok;
            switch (m->kind) {
                case METRIC_KIND_HISTOGRAM:
                    ok = format_histogram(buf, buf_len, &offset, m, ch);
                    break;
                case METRIC_KIND_COUNTER:
                    ok = format_counter(buf, buf_len, &offset, m, ch);
                    break;
                default:
                    ok = format_gauge(buf, buf_len, &offset, m, ch);
                    break;
            }
            if (!ok) {
                overflow = true;
                break;
            }
        }
        if (overflow) {
            break;
        }

        char escaped_error[METRICS_STORE_MAX_ERROR_LEN * 2];
        escape_label_value(ch->lua_error, escaped_error, sizeof(escaped_error));
        bool ok = (ch->lua_error[0] != '\0')
                      ? append(buf, buf_len, &offset,
                               "usb_bridge_channel_info{channel=\"%s\",status=\"%s\",lua_error=\"%s\"} 1\n",
                               ch->name, ch->online ? "online" : "offline", escaped_error)
                      : append(buf, buf_len, &offset, "usb_bridge_channel_info{channel=\"%s\",status=\"%s\"} 1\n",
                               ch->name, ch->online ? "online" : "offline");
        if (!ok) {
            overflow = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    return overflow ? -1 : (int)offset;
}

int metrics_store_format(char *buf, size_t buf_len) {
    return metrics_store_format_at(buf, buf_len, (uint32_t)xTaskGetTickCount());
}
