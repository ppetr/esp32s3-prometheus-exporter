#include "upload_auth.h"

#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#define UPLOAD_AUTH_NONCE_BYTES 16
#define UPLOAD_AUTH_SIGNATURE_BYTES 32
#define UPLOAD_AUTH_MAX_NONCES 8
#define UPLOAD_AUTH_NONCE_TTL_TICKS pdMS_TO_TICKS(5 * 60 * 1000)

typedef struct {
    bool valid;
    uint8_t bytes[UPLOAD_AUTH_NONCE_BYTES];
    uint32_t expiry_ticks;
} nonce_slot_t;

static SemaphoreHandle_t s_lock;
static nonce_slot_t s_nonces[UPLOAD_AUTH_MAX_NONCES];
static size_t s_next_slot;

static const char kHexDigits[] = "0123456789abcdef";

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = kHexDigits[bytes[i] >> 4];
        out[2 * i + 1] = kHexDigits[bytes[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    if (hex == NULL || strlen(hex) != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// HMAC-SHA256 key, imported once at init from the build-time secret - only
// valid when upload_auth_enabled() (there's nothing to import otherwise).
static mbedtls_svc_key_id_t s_hmac_key;
static bool s_hmac_key_valid;

esp_err_t upload_auth_init(void) {
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_nonces, 0, sizeof(s_nonces));
    s_next_slot = 0;

    if (upload_auth_enabled()) {
        psa_status_t status = psa_crypto_init();
        if (status == PSA_SUCCESS) {
            psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
            psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
            psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
            status = psa_import_key(&attr, (const uint8_t *)CONFIG_UPLOAD_AUTH_HMAC_KEY,
                                     strlen(CONFIG_UPLOAD_AUTH_HMAC_KEY), &s_hmac_key);
        }
        if (status != PSA_SUCCESS) {
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
            return ESP_FAIL;
        }
        s_hmac_key_valid = true;
    }
    return ESP_OK;
}

void upload_auth_deinit(void) {
    if (s_hmac_key_valid) {
        psa_destroy_key(s_hmac_key);
        s_hmac_key_valid = false;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
}

bool upload_auth_enabled(void) {
    return sizeof(CONFIG_UPLOAD_AUTH_HMAC_KEY) > 1;  // more than just the NUL of an empty string
}

esp_err_t upload_auth_issue_nonce(char *out_nonce_hex, uint32_t now_ticks) {
    if (!upload_auth_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t bytes[UPLOAD_AUTH_NONCE_BYTES];
    esp_fill_random(bytes, sizeof(bytes));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Round-robin eviction: a client that fetches a nonce and never uses it
    // just occupies a slot until overwritten - fine at this project's scale
    // (uploads are rare, manual actions, DESIGN.md §10).
    nonce_slot_t *slot = &s_nonces[s_next_slot];
    s_next_slot = (s_next_slot + 1) % UPLOAD_AUTH_MAX_NONCES;
    slot->valid = true;
    memcpy(slot->bytes, bytes, sizeof(bytes));
    slot->expiry_ticks = now_ticks + UPLOAD_AUTH_NONCE_TTL_TICKS;
    xSemaphoreGive(s_lock);

    bytes_to_hex(bytes, sizeof(bytes), out_nonce_hex);
    return ESP_OK;
}

bool upload_auth_verify_and_consume(const char *channel, const char *nonce_hex, const char *signature_hex,
                                     const uint8_t *body, size_t body_len, uint32_t now_ticks) {
    if (!upload_auth_enabled()) {
        return false;
    }

    uint8_t nonce_bytes[UPLOAD_AUTH_NONCE_BYTES];
    uint8_t want_sig[UPLOAD_AUTH_SIGNATURE_BYTES];
    if (!hex_to_bytes(nonce_hex, nonce_bytes, sizeof(nonce_bytes)) ||
        !hex_to_bytes(signature_hex, want_sig, sizeof(want_sig))) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    nonce_slot_t *found = NULL;
    for (size_t i = 0; i < UPLOAD_AUTH_MAX_NONCES; i++) {
        if (s_nonces[i].valid && memcmp(s_nonces[i].bytes, nonce_bytes, sizeof(nonce_bytes)) == 0) {
            found = &s_nonces[i];
            break;
        }
    }
    // Wraparound-safe "now before expiry": same idiom as sys_metrics' tick
    // delta handling, just as a signed before/after rather than a delta.
    bool nonce_ok = (found != NULL) && ((int32_t)(now_ticks - found->expiry_ticks) < 0);
    if (found != NULL) {
        found->valid = false;  // single-use: consume regardless of outcome below
    }
    xSemaphoreGive(s_lock);

    if (!nonce_ok) {
        return false;
    }

    // Signed string "<channel>,<nonce_hex>,<body>" (DESIGN.md §10) - binding
    // channel+nonce stops a nonce issued for one channel/request being
    // replayed against another.
    size_t channel_len = strlen(channel);
    size_t nonce_hex_len = strlen(nonce_hex);
    size_t msg_len = channel_len + 1 + nonce_hex_len + 1 + body_len;
    uint8_t *msg = malloc(msg_len);
    if (msg == NULL) {
        return false;
    }
    size_t off = 0;
    memcpy(msg + off, channel, channel_len);
    off += channel_len;
    msg[off++] = ',';
    memcpy(msg + off, nonce_hex, nonce_hex_len);
    off += nonce_hex_len;
    msg[off++] = ',';
    memcpy(msg + off, body, body_len);

    // psa_mac_verify() recomputes the HMAC itself and compares it in
    // constant time - no separate compare needed on our side.
    psa_status_t status = psa_mac_verify(s_hmac_key, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, msg_len, want_sig,
                                          sizeof(want_sig));
    free(msg);
    return status == PSA_SUCCESS;
}
