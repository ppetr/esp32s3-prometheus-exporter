#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed staleness threshold (DESIGN.md §6.3 point 4: a simple fixed default
// is acceptable for v1, not a user-facing config knob).
#define METRICS_STORE_STALENESS_MS 60000

// metrics_store_format() with an injectable tick count, so the
// staleness-omission logic above can be unit tested without waiting in real
// time. Not part of the public API - see metrics_store.h.
int metrics_store_format_at(char *buf, size_t buf_len, uint32_t now_ticks);

#ifdef __cplusplus
}
#endif
