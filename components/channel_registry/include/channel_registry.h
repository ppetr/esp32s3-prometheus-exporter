#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Capacity (DESIGN.md §9.1 expects a handful of channels). Fixed-size, no
// heap churn on the hot path.
#define CHANNEL_REGISTRY_MAX_CHANNELS 8
#define CHANNEL_REGISTRY_MAX_NAME_LEN 32

esp_err_t channel_registry_init(void);
void channel_registry_deinit(void);

// Registers a new channel with its own lua_sandbox_t (DESIGN.md §9.1's
// one-sandbox-per-channel model). Called when a device is detected
// (usb_host_bridge/i2c_poll_source) - a channel must exist before a script
// can be uploaded for it. Returns ESP_ERR_INVALID_STATE if the name is
// already registered, ESP_ERR_NO_MEM if out of channel slots.
esp_err_t channel_registry_add_channel(const char *channel, size_t max_memory_bytes);

// Unregisters a channel and destroys its sandbox (device disconnected).
esp_err_t channel_registry_remove_channel(const char *channel);

// Dry-run compiles source (against a throwaway state, not the channel's
// live one - DESIGN.md §10 point 1), then only on success atomically
// replaces the channel's active script via lua_sandbox_load(). Returns
// ESP_ERR_NOT_FOUND if the channel isn't registered, ESP_ERR_INVALID_ARG
// with err_buf filled on a syntax error.
esp_err_t channel_registry_load_script(const char *channel, const char *source, char *err_buf,
                                        size_t err_buf_len);

// Resumes the named channel's coroutine - a thin, locked passthrough to
// lua_sandbox_resume() (see lua_sandbox.h for parameter semantics).
// Returns ESP_ERR_NOT_FOUND if the channel isn't registered.
esp_err_t channel_registry_resume(const char *channel, const uint8_t *data, size_t len,
                                   int instruction_limit, uint32_t *yield_ms_out, char *err_buf,
                                   size_t err_buf_len);

#ifdef __cplusplus
}
#endif
