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

#include "self_doc.h"

#include <stdio.h>

#include "esp_log.h"
#include "self_doc_sizes.h"
#include "uzlib_inflate.h"

static const char *TAG = "self_doc";

// _start/_end are the linker symbols ESP-IDF's EMBED_FILES generates for
// each embedded file, named from its basename run through CMake's
// string(MAKE_C_IDENTIFIER ...) (dots become underscores) - verified
// against tools/cmake/scripts/data_file_embed_asm.cmake, not assumed.
extern const uint8_t manual_md_deflate_start[] asm("_binary_manual_md_deflate_start");
extern const uint8_t manual_md_deflate_end[] asm("_binary_manual_md_deflate_end");
extern const uint8_t manual_html_deflate_start[] asm("_binary_manual_html_deflate_start");
extern const uint8_t manual_html_deflate_end[] asm("_binary_manual_html_deflate_end");
extern const uint8_t openapi_yaml_start[] asm("_binary_openapi_yaml_start");
extern const uint8_t openapi_yaml_end[] asm("_binary_openapi_yaml_end");

// httpd_resp_send_chunk() always emits "Transfer-Encoding: chunked" - there
// is no esp_http_server API to stream a response in multiple writes under
// a caller-declared Content-Length other than building the header block by
// hand and writing the body directly via httpd_send() (DESIGN.md §11.1).
// httpd_send() itself doesn't loop on partial writes, unlike the private
// httpd_send_all() esp_http_server uses internally - hence this helper.
static esp_err_t send_all(httpd_req_t *req, const char *buf, size_t len) {
    while (len > 0) {
        int sent = httpd_send(req, buf, len);
        if (sent < 0) {
            return ESP_FAIL;
        }
        buf += sent;
        len -= (size_t)sent;
    }
    return ESP_OK;
}

static esp_err_t send_chunk_sink(const uint8_t *chunk, size_t len, void *ctx_v) {
    httpd_req_t *req = ctx_v;
    return send_all(req, (const char *)chunk, len);
}

// Sends `compressed` (DEFLATE, no gzip/zlib wrapper) decompressed, with a
// Content-Length declared up front from the build-time-known
// `original_size` - streamed straight into the socket as uzlib_inflate_raw
// produces it, never buffered whole in RAM (DESIGN.md §11.1).
static esp_err_t send_inflated(httpd_req_t *req, const char *content_type, const uint8_t *compressed,
                                size_t compressed_len, size_t original_size) {
    char header[160];
    int header_len = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n\r\n",
                               content_type, (unsigned)original_size);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    if (send_all(req, header, (size_t)header_len) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t err = uzlib_inflate_raw(compressed, compressed_len, send_chunk_sink, req);
    if (err != ESP_OK) {
        // Headers (and possibly some body bytes) are already on the wire;
        // nothing graceful left to do beyond logging and closing.
        ESP_LOGE(TAG, "inflate/send failed for %s: %s", req->uri, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t manual_html_get_handler(httpd_req_t *req) {
    return send_inflated(req, "text/html; charset=utf-8", manual_html_deflate_start,
                          (size_t)(manual_html_deflate_end - manual_html_deflate_start),
                          SELF_DOC_MANUAL_HTML_ORIGINAL_SIZE);
}

static esp_err_t manual_md_get_handler(httpd_req_t *req) {
    return send_inflated(req, "text/markdown; charset=utf-8", manual_md_deflate_start,
                          (size_t)(manual_md_deflate_end - manual_md_deflate_start), SELF_DOC_MANUAL_MD_ORIGINAL_SIZE);
}

// openapi.yaml is embedded uncompressed (EMBED_FILES, not EMBED_TXTFILES -
// no null terminator), so its length is exactly end - start; small enough
// that build-time compression wouldn't be worth the added complexity.
static esp_err_t openapi_yaml_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/yaml");
    return httpd_resp_send(req, (const char *)openapi_yaml_start, (ssize_t)(openapi_yaml_end - openapi_yaml_start));
}

esp_err_t self_doc_register(httpd_handle_t server) {
    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = manual_html_get_handler,
    };
    static const httpd_uri_t manual_md_uri = {
        .uri = "/manual.md",
        .method = HTTP_GET,
        .handler = manual_md_get_handler,
    };
    static const httpd_uri_t openapi_uri = {
        .uri = "/openapi.yaml",
        .method = HTTP_GET,
        .handler = openapi_yaml_get_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &root_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &manual_md_uri);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_register_uri_handler(server, &openapi_uri);
}
