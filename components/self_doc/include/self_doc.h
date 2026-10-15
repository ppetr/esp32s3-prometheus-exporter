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

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers GET /, /manual.md, /openapi.yaml on `server` (DESIGN.md §11.1):
// the device's own operator's manual (HTML and raw Markdown) and an
// OpenAPI spec for the rest of the HTTP API.
esp_err_t self_doc_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
