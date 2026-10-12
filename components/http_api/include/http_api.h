#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the HTTP server exposing /metrics and /scripts/<channel>
// (DESIGN.md §11). Runs on Core 0; never touches USB/I2C/Lua synchronously
// (DESIGN.md §6.2).
esp_err_t http_api_start(void);

// Request-handling logic for POST /scripts/<channel> (DESIGN.md §10),
// independent of the HTTP server itself: validates and atomically swaps a
// channel's script via channel_registry. The (not yet built) httpd
// handler parses the request body and calls this, then maps the result to
// an HTTP status (ESP_OK -> 200, ESP_ERR_INVALID_ARG -> 400 with err_buf as
// the body, ESP_ERR_NOT_FOUND -> 404). Persisting to LittleFS as
// parser_<channel>_new.lua before the swap is httpd/filesystem plumbing,
// out of scope here (DESIGN.md §12.2 - that's Tier 2/3).
esp_err_t http_api_handle_script_upload(const char *channel, const char *source, char *err_buf,
                                         size_t err_buf_len);

#ifdef __cplusplus
}
#endif
