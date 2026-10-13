#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hex string buffer sizes (including NUL) for the nonce and signature
// http_api passes to/from this module.
#define UPLOAD_AUTH_NONCE_HEX_LEN 33      // 16 random bytes
#define UPLOAD_AUTH_SIGNATURE_HEX_LEN 65  // HMAC-SHA256 output, 32 bytes

esp_err_t upload_auth_init(void);
void upload_auth_deinit(void);

// True if CONFIG_UPLOAD_AUTH_HMAC_KEY was set at build time. When false,
// nonce issuance and signature checking are fully inert (DESIGN.md §10) -
// callers should skip the nonce endpoint and the signature check on
// /scripts/* entirely rather than call the functions below.
bool upload_auth_enabled(void);

// Issues a fresh single-use nonce (DESIGN.md §10's server-issued,
// Withings-style nonce), valid for a few minutes from now_ticks. Writes its
// hex encoding (UPLOAD_AUTH_NONCE_HEX_LEN bytes incl. NUL) into
// out_nonce_hex. Returns ESP_ERR_INVALID_STATE if auth isn't enabled.
esp_err_t upload_auth_issue_nonce(char *out_nonce_hex, uint32_t now_ticks);

// Verifies signature_hex against HMAC-SHA256(secret,
// "<channel>,<nonce_hex>,<body>") and that nonce_hex is a still-valid,
// not-yet-used nonce from upload_auth_issue_nonce(). Binding the channel
// name into the signed string stops a nonce fetched for one channel being
// replayed against a different one. On success, consumes the nonce so the
// same signed request can't be replayed. Returns false if auth isn't
// enabled, either hex string is malformed, the signature doesn't match, or
// the nonce is unknown/expired/already used.
bool upload_auth_verify_and_consume(const char *channel, const char *nonce_hex, const char *signature_hex,
                                     const uint8_t *body, size_t body_len, uint32_t now_ticks);

#ifdef __cplusplus
}
#endif
