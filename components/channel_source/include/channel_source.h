#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Narrow interface between the scheduler/Lua pump and a data-producing
// channel (USB CDC, I2C poll, or a fake used for off-hardware testing).
// See DESIGN.md §12.1.
typedef struct {
    esp_err_t (*open)(void *ctx);
    // Returns bytes read, 0 on timeout, <0 on error/disconnect.
    int (*read)(void *ctx, uint8_t *buf, size_t maxlen, uint32_t timeout_ms);
    bool (*is_connected)(void *ctx);
    void *ctx;
} channel_source_t;

#ifdef __cplusplus
}
#endif
