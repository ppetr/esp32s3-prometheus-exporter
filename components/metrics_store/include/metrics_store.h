#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t metrics_store_init(void);

// Called from the Lua pump (Core 1). Updates last_update_tick internally
// (DESIGN.md §6.1).
esp_err_t metrics_store_set(const char *channel, const char *metric_name, float value);

// Always-present per-channel health metric (DESIGN.md §6.3).
esp_err_t metrics_store_set_channel_status(const char *channel, bool online, const char *lua_error);

// Called from the HTTP handler (Core 0). Writes Prometheus text exposition
// format into buf, omitting metrics older than the staleness threshold
// (DESIGN.md §6.3). Returns bytes written, or <0 if buf was too small.
int metrics_store_format(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
