#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Capacity limits (DESIGN.md §6.1 expects a handful of channels/metrics per
// channel; fixed-size arrays avoid heap churn on Core 1's hot path for the
// channel/metric slots themselves). Each metric slot is independently either
// a gauge or a histogram - the two kinds share this one pool rather than
// having separate per-kind limits, since the split between them isn't known
// ahead of time.
#define METRICS_STORE_MAX_CHANNELS 8
#define METRICS_STORE_MAX_METRICS_PER_CHANNEL 16
#define METRICS_STORE_MAX_NAME_LEN 32

// Sanity ceiling on a single histogram's bucket count. Bucket storage is
// allocated once, sized exactly to what the caller asks for in
// metrics_store_histogram_init() - this bound only guards against a runaway
// allocation request, it is not a fixed array size.
#define METRICS_STORE_MAX_HISTOGRAM_BUCKETS 128

esp_err_t metrics_store_init(void);

// Frees any histogram bucket storage, clears all state, and releases the
// lock. Mainly useful for tests; production firmware has no normal reason to
// call this.
void metrics_store_deinit(void);

// Called from the Lua pump (Core 1). Updates last_update_tick internally
// (DESIGN.md §6.1). Creates the channel/metric entry on first use. Returns
// ESP_ERR_NO_MEM if out of channel or metric slots, ESP_ERR_INVALID_STATE if
// not initialized or if metric_name already names a histogram on this
// channel.
esp_err_t metrics_store_set(const char *channel, const char *metric_name, float value);

// Monotonically increasing counter (DESIGN.md §6.6). Creates the entry at 0
// on first use. amount must be >= 0 - counters never decrease (a process
// restart naturally resets to 0, which Prometheus's own rate()/increase()
// already handle as a counter reset). By Prometheus convention, name the
// metric itself with a trailing "_total" (e.g. "bytes_received_total") -
// this function does not add the suffix for you. Returns
// ESP_ERR_INVALID_STATE if metric_name already names a non-counter metric
// on this channel.
esp_err_t metrics_store_counter_add(const char *channel, const char *metric_name, double amount);

// Defines (or redefines) a histogram's bucket upper bounds, which must be
// strictly ascending; an implicit "+Inf" bucket catches every observation.
// Must be called once before metrics_store_histogram_observe() for this
// channel/metric_name pair - redefining an existing histogram resets its
// counts. Returns ESP_ERR_INVALID_ARG for a bad bucket_count/ordering,
// ESP_ERR_INVALID_STATE if metric_name already names a non-histogram metric
// on this channel.
esp_err_t metrics_store_histogram_init(const char *channel, const char *metric_name,
                                        const float *bucket_upper_bounds, size_t bucket_count);

// Records one observation into a histogram previously defined with
// metrics_store_histogram_init(): increments the matching bucket, the total
// count, and the running sum. Returns ESP_ERR_NOT_FOUND if the channel or
// histogram hasn't been defined yet.
esp_err_t metrics_store_histogram_observe(const char *channel, const char *metric_name, float value);

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
