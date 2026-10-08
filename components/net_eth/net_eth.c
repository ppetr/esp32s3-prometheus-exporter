#include "net_eth.h"

#include "esp_log.h"

static const char *TAG = "net_eth";

// Not yet implemented: W5500 esp_eth bring-up and SNTP (DESIGN.md §6.4,
// §9 "Implementation detail for S3 and W5500"). Hardware-only; not a Tier 1
// target. Confirm the W5500 SPI bus assignment against the real Waveshare
// schematic before wiring this up (DESIGN.md §13).

esp_err_t net_eth_init(void) {
    ESP_LOGW(TAG, "net_eth_init: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

bool net_eth_time_synchronized(void) {
    return false;
}
