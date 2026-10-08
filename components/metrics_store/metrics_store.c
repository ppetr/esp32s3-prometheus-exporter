#include "metrics_store.h"

#include "esp_log.h"

static const char *TAG = "metrics_store";

// Not yet implemented: the mutex/atomic-swap-protected metric map and
// staleness-omission logic from DESIGN.md §6.2-§6.3. This is the main
// candidate for Tier 1 host unit tests (DESIGN.md §12.2), being entirely
// hardware-independent.

esp_err_t metrics_store_init(void) {
    ESP_LOGW(TAG, "metrics_store_init: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t metrics_store_set(const char *channel, const char *metric_name, float value) {
    (void)channel;
    (void)metric_name;
    (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t metrics_store_set_channel_status(const char *channel, bool online, const char *lua_error) {
    (void)channel;
    (void)online;
    (void)lua_error;
    return ESP_ERR_NOT_SUPPORTED;
}

int metrics_store_format(char *buf, size_t buf_len) {
    (void)buf;
    (void)buf_len;
    return -1;
}
