#pragma once

#include "channel_source.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Real channel_source_t backed by USB Host CDC-ACM (DESIGN.md §8).
// Deliberately thin: moves bytes, does no parsing or scheduling.
esp_err_t usb_host_bridge_init(void);
esp_err_t usb_host_bridge_get_source(channel_source_t *out_source);

#ifdef __cplusplus
}
#endif
