/*
 * Copyright 2026 https://github.com/ppetr/esp32s3-prometheus-exporter
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Invoked with each produced chunk of decompressed output, in order; the
// sum of `len` across all calls equals the decompressed size. Returning
// anything other than ESP_OK aborts decompression with that error.
typedef esp_err_t (*uzlib_inflate_sink_t)(const uint8_t *chunk, size_t len, void *ctx);

// Inflates a raw DEFLATE stream (RFC 1951; no gzip/zlib container, no
// checksum) - e.g. produced by Python's zlib.compressobj(wbits=-15) - and
// hands the output to `sink` in bounded-size chunks as it's produced,
// rather than holding the whole decompressed document in memory at once
// (DESIGN.md §11.1). `src`/`src_len` must stay valid for the call.
esp_err_t uzlib_inflate_raw(const uint8_t *src, size_t src_len, uzlib_inflate_sink_t sink, void *ctx);

#ifdef __cplusplus
}
#endif
