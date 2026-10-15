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

#include "uzlib_inflate.h"

#include <stdlib.h>
#include <string.h>

#include "uzlib.h"

// DEFLATE's maximum back-reference distance (RFC 1951 §3.2.5). The sliding
// window must cover this so a back-reference into output already flushed
// out of the (much smaller) chunk buffer still resolves correctly.
#define UZLIB_INFLATE_WINDOW_SIZE 32768
#define UZLIB_INFLATE_CHUNK_SIZE 1024

esp_err_t uzlib_inflate_raw(const uint8_t *src, size_t src_len, uzlib_inflate_sink_t sink, void *ctx) {
    uint8_t *window = malloc(UZLIB_INFLATE_WINDOW_SIZE);
    uint8_t *chunk = malloc(UZLIB_INFLATE_CHUNK_SIZE);
    if (window == NULL || chunk == NULL) {
        free(window);
        free(chunk);
        return ESP_ERR_NO_MEM;
    }

    TINF_DATA d;
    memset(&d, 0, sizeof(d));
    uzlib_uncompress_init(&d, window, UZLIB_INFLATE_WINDOW_SIZE);
    d.source = src;
    d.source_limit = src + src_len;

    esp_err_t err = ESP_OK;
    int res;
    do {
        d.dest = chunk;
        d.dest_start = chunk;
        d.dest_limit = chunk + UZLIB_INFLATE_CHUNK_SIZE;
        res = uzlib_uncompress(&d);

        size_t produced = (size_t)(d.dest - chunk);
        if (produced > 0) {
            err = sink(chunk, produced, ctx);
            if (err != ESP_OK) {
                break;
            }
        }
    } while (res == TINF_OK);

    free(window);
    free(chunk);

    if (err != ESP_OK) {
        return err;
    }
    return (res == TINF_DONE) ? ESP_OK : ESP_FAIL;
}
