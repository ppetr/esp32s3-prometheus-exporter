#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the HTTP server exposing /metrics and /scripts/<channel>
// (DESIGN.md §11). Runs on Core 0; never touches USB/I2C/Lua synchronously
// (DESIGN.md §6.2).
esp_err_t http_api_start(void);

#ifdef __cplusplus
}
#endif
