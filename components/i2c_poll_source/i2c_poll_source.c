#include "i2c_poll_source.h"

#include "esp_log.h"

static const char *TAG = "i2c_poll_source";

// Not yet implemented: I2C master bus init and per-device register read,
// per DESIGN.md §9.1 and the open item on confirming a free SPI/I2C bus
// (DESIGN.md §13).
esp_err_t i2c_poll_source_init(void) {
    ESP_LOGW(TAG, "i2c_poll_source_init: not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t i2c_poll_source_get_source(uint8_t device_addr, channel_source_t *out_source) {
    (void)device_addr;
    (void)out_source;
    return ESP_ERR_NOT_SUPPORTED;
}
