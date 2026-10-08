#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up Ethernet (W5500 over SPI on real hardware, DESIGN.md §2) and
// attaches it to LwIP, then starts best-effort SNTP (DESIGN.md §6.4). The
// Tier 2 QEMU build variant swaps the W5500-specific bring-up for QEMU's
// emulated netif here (DESIGN.md §12.3, §13).
esp_err_t net_eth_init(void);

// True once SNTP has completed at least one sync; gates whether the HTTP
// exporter appends a Unix timestamp (DESIGN.md §6.4).
bool net_eth_time_synchronized(void);

#ifdef __cplusplus
}
#endif
