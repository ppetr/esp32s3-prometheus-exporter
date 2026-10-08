#include "usb_host_bridge.h"

#include "esp_log.h"

static const char *TAG = "usb_host_bridge";

// Not yet implemented: USB Host install + CDC-ACM driver bring-up
// (ESP-IDF usb_host_cdc_acm component), per DESIGN.md §8-§9.1.
esp_err_t usb_host_bridge_init(void) {
    ESP_LOGW(TAG, "usb_host_bridge_init: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t usb_host_bridge_get_source(channel_source_t *out_source) {
    (void)out_source;
    return ESP_ERR_NOT_SUPPORTED;
}
