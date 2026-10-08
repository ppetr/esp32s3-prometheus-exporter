#pragma once

#include "channel_source.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Real channel_source_t backed by periodic I2C register reads
// (DESIGN.md §9.1, poll-driven parser shape). Thin by design: moves bytes,
// does no parsing or scheduling.
esp_err_t i2c_poll_source_init(void);
esp_err_t i2c_poll_source_get_source(uint8_t device_addr, channel_source_t *out_source);

#ifdef __cplusplus
}
#endif
