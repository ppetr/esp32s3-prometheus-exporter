#include "http_api.h"

#include "channel_registry.h"
#include "esp_log.h"

static const char *TAG = "http_api";

// Not yet implemented: esp_http_server setup with the /metrics and
// /scripts/<channel> handlers from DESIGN.md §10-§11. The handler logic
// itself is implemented below (http_api_handle_script_upload) and is a
// Tier 1 host-test target (DESIGN.md §12.2); only the httpd plumbing
// (routing requests here, persisting to LittleFS) is still Tier 2/3 only.
esp_err_t http_api_start(void) {
    ESP_LOGW(TAG, "http_api_start: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t http_api_handle_script_upload(const char *channel, const char *source, char *err_buf,
                                         size_t err_buf_len) {
    return channel_registry_load_script(channel, source, err_buf, err_buf_len);
}
