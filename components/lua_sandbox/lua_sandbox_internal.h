#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "lua_sandbox.h"

#ifdef __cplusplus
extern "C" {
#endif

// Test-only seam (same pattern as metrics_store_internal.h): lets Tier 1
// tests observe a script's effect on its own globals without a full
// channel_source/metrics_store wiring (set_metric() et al. don't exist yet -
// that's the Lua pump's job, not this component's). Not part of the public
// API - see lua_sandbox.h.
esp_err_t lua_sandbox_test_get_global_number(lua_sandbox_t *sandbox, const char *name, double *out);
esp_err_t lua_sandbox_test_get_global_string(lua_sandbox_t *sandbox, const char *name, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
