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

// Per-channel pending-data buffer (DESIGN.md §8's "256-512 byte range").
// Fixed-size and non-blocking rather than a true ring buffer with a
// blocking wait: at this project's scale (single-digit channels, the Lua
// pump polling every CHANNEL_REGISTRY_POLL... see lua_pump) a full buffer
// is a push-side backpressure signal, same role as the real USB
// hardware's own NAK (DESIGN.md §8), not a queue depth to tune.
#define CHANNEL_REGISTRY_PENDING_DATA_BYTES 512

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
// lua_sandbox_resume() (see lua_sandbox.h for parameter semantics). On a
// successful resume, also records the tick it ran at (for
// channel_registry_recover_stalled_channels()) and, from *yield_ms_out,
// when it's next due (for channel_registry_pick_due_channel()) - both
// using the real clock, not a caller-supplied tick. Returns
// ESP_ERR_NOT_FOUND if the channel isn't registered.
esp_err_t channel_registry_resume(const char *channel, const uint8_t *data, size_t len,
                                   int instruction_limit, uint32_t *yield_ms_out, char *err_buf,
                                   size_t err_buf_len);

// Picks the channel most overdue to run (DESIGN.md §9.1's "earliest due
// coroutine" selection): the registered channel with the smallest
// next_run_ticks that is <= now_ticks. A freshly added channel, or one
// whose last resume didn't request a delay (yield_ms was 0 - e.g. a
// stream-driven yield), is always due.
//
// On success, copies the due channel's name into out_channel (size
// out_channel_len) and returns ESP_OK. If no channel is due yet, returns
// ESP_ERR_NOT_FOUND and sets *out_wait_ticks to how long until the
// earliest one becomes due (so the pump loop knows how long it can sleep
// rather than busy-poll); if no channel is registered at all, returns
// ESP_ERR_NOT_FOUND and leaves *out_wait_ticks untouched.
esp_err_t channel_registry_pick_due_channel(uint32_t now_ticks, char *out_channel, size_t out_channel_len,
                                             uint32_t *out_wait_ticks);

// Finds channels whose coroutine hasn't been successfully resumed in over
// stall_ticks (DESIGN.md §9.2's logical timeout for a stalled mid-message
// coroutine - e.g. a device unplugged mid-message, so nothing ever
// triggers another resume) and discards + recreates their coroutine from
// the last successfully loaded script. Channels that have never had a
// script loaded are never considered stalled - there would be nothing to
// recreate. Returns the number of channels recovered this way.
int channel_registry_recover_stalled_channels(uint32_t now_ticks, uint32_t stall_ticks);

// Appends bytes for a channel to drain on its next resume (DESIGN.md §9's
// producer/consumer split: called from a different task than the one
// driving resume - a future USB Host CDC task - so neither ever blocks on
// the other). Non-blocking: returns ESP_ERR_NO_MEM if the channel's
// CHANNEL_REGISTRY_PENDING_DATA_BYTES buffer doesn't have room right now;
// the caller decides whether to retry or drop. Returns ESP_ERR_NOT_FOUND
// if the channel isn't registered.
esp_err_t channel_registry_push_data(const char *channel, const uint8_t *data, size_t len);

// Finds any one registered channel with non-empty pending data (pushed via
// channel_registry_push_data()) and drains it directly into buf, so the
// Lua pump doesn't need to enumerate channels itself to find one. Copies
// the drained channel's name into out_channel. Returns the number of
// bytes drained (0 if no channel currently has pending data), or <0 if
// out_channel/buf are invalid.
int channel_registry_pick_channel_with_data(char *out_channel, size_t out_channel_len, uint8_t *buf, size_t maxlen);

#ifdef __cplusplus
}
#endif
