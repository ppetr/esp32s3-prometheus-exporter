#include "http_api.h"

#include "esp_log.h"

static const char *TAG = "http_api";

// Not yet implemented: esp_http_server setup with the /metrics and
// /scripts/<channel> handlers from DESIGN.md §10-§11. The handler logic
// (upload validation, atomic swap decision) is a Tier 1 host-test target
// (DESIGN.md §12.2); the httpd plumbing itself is Tier 2/3 only.
esp_err_t http_api_start(void) {
    ESP_LOGW(TAG, "http_api_start: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}
