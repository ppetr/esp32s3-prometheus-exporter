#include "http_api.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel_registry.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "metrics_store.h"

static const char *TAG = "http_api";

#define HTTP_API_LITTLEFS_BASE_PATH "/littlefs"
#define HTTP_API_LITTLEFS_PARTITION "storage"

// Generous but bounded (DESIGN.md §6.1's "handful of channels/metrics"
// scale); metrics_store_format() reports -1 on truncation rather than
// overflowing, so a too-small buffer fails loudly instead of corrupting.
#define HTTP_API_METRICS_BUF_SIZE 8192

// Upper bound on one uploaded Lua script's size. Generous for a parser
// script; just a sanity cap against a client streaming an unbounded body.
#define HTTP_API_MAX_SCRIPT_BYTES 8192

static httpd_handle_t s_server;

static esp_err_t metrics_get_handler(httpd_req_t *req) {
    char *buf = malloc(HTTP_API_METRICS_BUF_SIZE);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    int len = metrics_store_format(buf, HTTP_API_METRICS_BUF_SIZE);
    if (len < 0) {
        free(buf);
        ESP_LOGE(TAG, "metrics_store_format: buffer too small");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metrics buffer too small");
    }

    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

// Builds "<base>/parser_<channel>.lua" (or "..._new.lua" if new_suffix).
static void script_path(char *out, size_t out_len, const char *channel, bool new_suffix) {
    snprintf(out, out_len, "%s/parser_%s%s.lua", HTTP_API_LITTLEFS_BASE_PATH, channel, new_suffix ? "_new" : "");
}

// Persists a validated script to flash (DESIGN.md §10 points 2-3): write
// to a "_new" file, then atomically rename it over the active one. Called
// BEFORE the live swap (channel_registry_validate_script() has already
// run by this point, never channel_registry_load_script()) specifically
// so a failure here can be reported as a clean upload failure with no
// live effect at all, rather than a channel silently running a script
// that won't survive its next reboot.
static esp_err_t persist_script(const char *channel, const char *source, size_t len) {
    char new_path[96];
    char active_path[96];
    script_path(new_path, sizeof(new_path), channel, true);
    script_path(active_path, sizeof(active_path), channel, false);

    FILE *f = fopen(new_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "persist_script: failed to open %s for writing", new_path);
        return ESP_FAIL;
    }
    size_t written = fwrite(source, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "persist_script: short write to %s", new_path);
        remove(new_path);
        return ESP_FAIL;
    }

    remove(active_path);  // LittleFS rename() doesn't overwrite an existing destination.
    if (rename(new_path, active_path) != 0) {
        ESP_LOGE(TAG, "persist_script: rename %s -> %s failed", new_path, active_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t scripts_post_handler(httpd_req_t *req) {
    static const char kPrefix[] = "/scripts/";
    const char *channel = req->uri + sizeof(kPrefix) - 1;
    size_t channel_len = strlen(channel);
    if (channel_len == 0 || channel_len >= CHANNEL_REGISTRY_MAX_NAME_LEN || strchr(channel, '/') != NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid channel name in URI");
    }

    if (req->content_len == 0 || req->content_len >= HTTP_API_MAX_SCRIPT_BYTES) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "script body missing or too large");
    }

    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            free(body);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, NULL);
            }
            return ESP_FAIL;
        }
        received += (size_t)n;
    }
    body[received] = '\0';

    // Validate -> persist -> swap, in that order (DESIGN.md §10): nothing
    // about the channel's live state changes until the script is already
    // durable on flash, so a persist failure is a clean upload failure,
    // never a channel silently running something that won't survive its
    // next reboot.
    char err_buf[128] = {0};
    esp_err_t err = channel_registry_validate_script(channel, body, err_buf, sizeof(err_buf));

    esp_err_t send_err;
    switch (err) {
        case ESP_OK:
            if (persist_script(channel, body, received) != ESP_OK) {
                send_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to persist script");
                break;
            }
            // Already validated above; this repeats that check but is the
            // only call that actually swaps the live coroutine in.
            err = http_api_handle_script_upload(channel, body, err_buf, sizeof(err_buf));
            send_err = (err == ESP_OK) ? httpd_resp_sendstr(req, "OK")
                                        : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                               "persisted but failed to apply live");
            break;
        case ESP_ERR_INVALID_ARG:
            httpd_resp_set_status(req, "400 Bad Request");
            send_err = httpd_resp_sendstr(req, err_buf);
            break;
        case ESP_ERR_NOT_FOUND:
            send_err = httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown channel");
            break;
        default:
            send_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
            break;
    }

    free(body);
    return send_err;
}

static esp_err_t mount_littlefs(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = HTTP_API_LITTLEFS_BASE_PATH,
        .partition_label = HTTP_API_LITTLEFS_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t http_api_start(void) {
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = mount_littlefs();
    if (err != ESP_OK) {
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t metrics_uri = {
        .uri = "/metrics",
        .method = HTTP_GET,
        .handler = metrics_get_handler,
    };
    static const httpd_uri_t scripts_uri = {
        .uri = "/scripts/*",
        .method = HTTP_POST,
        .handler = scripts_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &metrics_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &scripts_uri));

    return ESP_OK;
}

esp_err_t http_api_handle_script_upload(const char *channel, const char *source, char *err_buf,
                                         size_t err_buf_len) {
    return channel_registry_load_script(channel, source, err_buf, err_buf_len);
}
