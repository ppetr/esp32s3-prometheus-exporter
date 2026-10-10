#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Capacity limits (DESIGN.md §6.1 expects a handful of channels/metrics per
// channel; fixed-size arrays avoid heap churn on Core 1's hot path).
#define METRICS_STORE_MAX_CHANNELS 8
#define METRICS_STORE_MAX_METRICS_PER_CHANNEL 16
#define METRICS_STORE_MAX_NAME_LEN 32

esp_err_t metrics_store_init(void);

// Clears all state and releases the lock. Mainly useful for tests;
// production firmware has no normal reason to call this.
void metrics_store_deinit(void);

// Called from the Lua pump (Core 1). Updates last_update_tick internally
// (DESIGN.md §6.1). Creates the channel/metric entry on first use. Returns
// ESP_ERR_NO_MEM if out of channel or metric slots, ESP_ERR_INVALID_STATE if
// not initialized.
esp_err_t metrics_store_set(const char *channel, const char *metric_name, float value);

// Always-present per-channel health metric (DESIGN.md §6.3). Creates the
// channel entry on first use.
esp_err_t metrics_store_set_channel_status(const char *channel, bool online, const char *lua_error);

// Called from the HTTP handler (Core 0). Writes Prometheus text exposition
// format into buf, omitting metrics older than the staleness threshold
// (DESIGN.md §6.3). Returns bytes written, or <0 if buf was too small.
int metrics_store_format(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
