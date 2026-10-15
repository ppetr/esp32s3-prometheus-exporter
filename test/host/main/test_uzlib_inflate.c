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

#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "uzlib_inflate.h"

// Repeated 31-byte phrase, 40x (1240 bytes) - repetitive enough that
// zlib's compressor actually emits LZ77 back-references (not just literals),
// and long enough to span more than one of uzlib_inflate_raw's internal
// 1024-byte output chunks, exercising the sliding-window ring across that
// chunk boundary (DESIGN.md §11.1).
static const char kExpected[] =
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! "
    "Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! Hello, self-documenting ESP32! ";

// Generated via: zlib.compressobj(level=9, wbits=-15).compress(kExpected)
// - i.e. raw DEFLATE, no gzip/zlib wrapper (DESIGN.md §11.1's chosen
// on-flash format for the manual and its generated HTML).
static const uint8_t kCompressed[] = {
    0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x28, 0x4e, 0xcd, 0x49, 0xd3, 0x4d, 0xc9, 0x4f, 0x2e,
    0xcd, 0x4d, 0xcd, 0x2b, 0xc9, 0xcc, 0x4b, 0x57, 0x70, 0x0d, 0x0e, 0x30, 0x36, 0x52, 0x54, 0xf0,
    0x18, 0x95, 0x1e, 0x95, 0x1e, 0x95, 0x1e, 0x95, 0x1e, 0x6a, 0xd2, 0x00,
};

struct collect_ctx {
    uint8_t *buf;
    size_t len;
    size_t cap;
};

static esp_err_t collect_sink(const uint8_t *chunk, size_t chunk_len, void *ctx_v) {
    struct collect_ctx *ctx = ctx_v;
    if (ctx->len + chunk_len > ctx->cap) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(ctx->buf + ctx->len, chunk, chunk_len);
    ctx->len += chunk_len;
    return ESP_OK;
}

void test_uzlib_inflate_raw_round_trips_known_vector(void) {
    uint8_t out[sizeof(kExpected) - 1];
    struct collect_ctx ctx = {.buf = out, .len = 0, .cap = sizeof(out)};

    esp_err_t err = uzlib_inflate_raw(kCompressed, sizeof(kCompressed), collect_sink, &ctx);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(sizeof(out), ctx.len);
    TEST_ASSERT_EQUAL_MEMORY(kExpected, out, sizeof(out));
}

static esp_err_t fail_after_first_sink(const uint8_t *chunk, size_t chunk_len, void *ctx_v) {
    (void)chunk;
    (void)chunk_len;
    int *calls = ctx_v;
    (*calls)++;
    return ESP_FAIL;
}

void test_uzlib_inflate_raw_propagates_sink_error(void) {
    int calls = 0;
    esp_err_t err = uzlib_inflate_raw(kCompressed, sizeof(kCompressed), fail_after_first_sink, &calls);

    TEST_ASSERT_EQUAL(ESP_FAIL, err);
    TEST_ASSERT_EQUAL(1, calls);
}

void test_uzlib_inflate_raw_truncated_input_returns_fail(void) {
    uint8_t out[sizeof(kExpected) - 1];
    struct collect_ctx ctx = {.buf = out, .len = 0, .cap = sizeof(out)};

    // Missing the final bytes (including the final-block flag): a decoder
    // that doesn't track its own end-of-stream state could otherwise read
    // past the end of the source buffer here.
    esp_err_t err = uzlib_inflate_raw(kCompressed, sizeof(kCompressed) - 4, collect_sink, &ctx);

    TEST_ASSERT_EQUAL(ESP_FAIL, err);
}
