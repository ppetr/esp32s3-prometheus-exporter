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

// No setUp/tearDown here - test_metrics_store.c already defines those.
//
// Exercises self_doc's three routes (DESIGN.md §11.1) over a real TCP
// socket against a real httpd_server, on the ESP-IDF Linux target - proven
// to work (esp_http_server/lwip build and serve real sockets there) by a
// manual smoke test before writing this; this is that smoke test made
// permanent and automated. Deliberately checks the *contract* (status,
// Content-Type, Content-Length matching the bytes actually received, the
// two discovery links) rather than diffing the full body against
// docs/manual.md - that would make every prose edit break this test for no
// reason.

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "self_doc.h"
#include "unity.h"

#define TEST_PORT 18181
#define RESP_BUF_SIZE 16384

static httpd_handle_t s_server;

// Starts the server once, lazily, and leaves it running - self_doc has no
// mutable device-side state to reset between tests, and the whole test
// binary exits (via UNITY_END()) right after the last test anyway.
static void ensure_server_started(void) {
    if (s_server != NULL) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = TEST_PORT;
    TEST_ASSERT_EQUAL(ESP_OK, httpd_start(&s_server, &config));
    TEST_ASSERT_EQUAL(ESP_OK, self_doc_register(s_server));
}

// connect()/send()/recv() on the ESP-IDF Linux target can all return -1/
// EINTR (FreeRTOS's Linux port ticks via a periodic signal, which
// interrupts blocking syscalls) well before failing or reaching EOF for
// real - confirmed by instrumenting a failing run before writing this.
// Worse than a plain "retry the syscall": busy-retrying without yielding
// never lets httpd's own worker task (a separate FreeRTOS task, but this
// port still schedules cooperatively enough that a tight native-syscall
// retry loop can starve it) actually run and service the request - also
// confirmed the same way. vTaskDelay(1) between attempts fixes that.
#define RETRY_ON_EINTR(call_expr, max_attempts)                   \
    ({                                                             \
        __typeof__(call_expr) _result;                            \
        int _attempt = 0;                                         \
        do {                                                      \
            _result = (call_expr);                                \
            if (_result >= 0 || errno != EINTR) {                 \
                break;                                             \
            }                                                     \
            vTaskDelay(1);                                        \
        } while (++_attempt < (max_attempts));                    \
        _result;                                                  \
    })
#define MAX_EINTR_RETRIES 200

static ssize_t recv_retry(int sock, void *buf, size_t len) {
    return RETRY_ON_EINTR(recv(sock, buf, len, 0), MAX_EINTR_RETRIES);
}

// Issues "GET <path> HTTP/1.1" and splits the raw response into
// (status_line, headers, body) in out_* (all pointers into resp_buf, not
// separately allocated). Returns the total bytes read.
static size_t http_get(const char *path, char *resp_buf, size_t resp_buf_size, char **out_status_line,
                        char **out_headers, char **out_body, size_t *out_body_len) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(sock >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    TEST_ASSERT_EQUAL(1, inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr));
    TEST_ASSERT_EQUAL(0, RETRY_ON_EINTR(connect(sock, (struct sockaddr *)&addr, sizeof(addr)), MAX_EINTR_RETRIES));

    char req[256];
    int req_len = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
    TEST_ASSERT_EQUAL(req_len, RETRY_ON_EINTR(send(sock, req, (size_t)req_len, 0), MAX_EINTR_RETRIES));

    size_t total = 0;
    while (total < resp_buf_size - 1) {
        ssize_t n = recv_retry(sock, resp_buf + total, resp_buf_size - 1 - total);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    close(sock);
    resp_buf[total] = '\0';

    char *header_end = strstr(resp_buf, "\r\n\r\n");
    TEST_ASSERT_NOT_NULL(header_end);
    *out_body = header_end + 4;
    *out_body_len = total - (size_t)(*out_body - resp_buf);
    *header_end = '\0';  // terminates the headers region (searched below); body is unaffected, 4 bytes later

    char *first_line_end = strstr(resp_buf, "\r\n");
    TEST_ASSERT_NOT_NULL(first_line_end);
    *first_line_end = '\0';
    *out_status_line = resp_buf;
    *out_headers = first_line_end + 2;

    return total;
}

// Finds "Content-Length: <n>" among the (NUL-terminated) headers.
static long find_content_length(const char *headers) {
    const char *p = strstr(headers, "Content-Length:");
    TEST_ASSERT_NOT_NULL(p);
    return strtol(p + strlen("Content-Length:"), NULL, 10);
}

void test_self_doc_manual_md_served_correctly(void) {
    ensure_server_started();

    char resp[RESP_BUF_SIZE];
    char *status_line, *headers, *body;
    size_t body_len;
    http_get("/manual.md", resp, sizeof(resp), &status_line, &headers, &body, &body_len);

    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 200 OK", status_line);
    TEST_ASSERT_NOT_NULL(strstr(headers, "Content-Type: text/markdown; charset=utf-8"));
    TEST_ASSERT_EQUAL((long)body_len, find_content_length(headers));
    TEST_ASSERT_EQUAL_STRING_LEN("# ESP32-S3 Telemetry Bridge", body, strlen("# ESP32-S3 Telemetry Bridge"));
}

void test_self_doc_root_served_as_html_with_discovery_links(void) {
    ensure_server_started();

    char resp[RESP_BUF_SIZE];
    char *status_line, *headers, *body;
    size_t body_len;
    http_get("/", resp, sizeof(resp), &status_line, &headers, &body, &body_len);

    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 200 OK", status_line);
    TEST_ASSERT_NOT_NULL(strstr(headers, "Content-Type: text/html; charset=utf-8"));
    TEST_ASSERT_EQUAL((long)body_len, find_content_length(headers));
    TEST_ASSERT_EQUAL_STRING_LEN("<!DOCTYPE html>", body, strlen("<!DOCTYPE html>"));
    // Both discovery mechanisms from DESIGN.md §11.1: the <head> <link> and
    // the plain visible body link, for HTML-to-text pipelines that drop
    // <head>.
    TEST_ASSERT_NOT_NULL(strstr(body, "<link rel=\"alternate\" type=\"text/markdown\" href=\"/manual.md\">"));
    TEST_ASSERT_NOT_NULL(strstr(body, "<a href=\"/manual.md\">"));
}

void test_self_doc_openapi_yaml_served_correctly(void) {
    ensure_server_started();

    char resp[RESP_BUF_SIZE];
    char *status_line, *headers, *body;
    size_t body_len;
    http_get("/openapi.yaml", resp, sizeof(resp), &status_line, &headers, &body, &body_len);

    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 200 OK", status_line);
    TEST_ASSERT_NOT_NULL(strstr(headers, "Content-Type: application/yaml"));
    TEST_ASSERT_EQUAL((long)body_len, find_content_length(headers));
    TEST_ASSERT_NOT_NULL(strstr(body, "externalDocs"));
    TEST_ASSERT_NOT_NULL(strstr(body, "/manual.md"));
}

void test_self_doc_unknown_path_returns_404(void) {
    ensure_server_started();

    char resp[RESP_BUF_SIZE];
    char *status_line, *headers, *body;
    size_t body_len;
    http_get("/does-not-exist", resp, sizeof(resp), &status_line, &headers, &body, &body_len);

    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 404 Not Found", status_line);
}
