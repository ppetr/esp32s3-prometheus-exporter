#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Reads live heap/PSRAM/CPU/stack usage and pushes it into metrics_store
// (DESIGN.md §7). Call once per /metrics scrape, right before
// metrics_store_format() - CPU usage needs a previous sample to compute a
// delta, so the first call after boot only establishes a baseline and
// won't appear in the exposition yet.
void sys_metrics_refresh(void);

#ifdef __cplusplus
}
#endif
