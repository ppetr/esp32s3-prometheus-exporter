#include "upload_auth.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/psa_util.h"
#include "psa/crypto.h"

static const char *TAG = "upload_auth";

#define UPLOAD_AUTH_NONCE_BYTES 16
#define UPLOAD_AUTH_PUBKEY_BYTES 65          // SEC1 uncompressed P-256 point: 0x04 || X || Y
#define UPLOAD_AUTH_SIGNATURE_RAW_BYTES 64   // PSA's fixed-width r||s for P-256
#define UPLOAD_AUTH_SIGNATURE_DER_MAX_BYTES 72
#define UPLOAD_AUTH_MAX_NONCES 8
#define UPLOAD_AUTH_NONCE_TTL_TICKS pdMS_TO_TICKS(5 * 60 * 1000)

// upload_auth.h mirrors this bound in its own hex buffer size so http_api
// doesn't need to pull in mbedtls/psa_util.h just to size a header buffer.
_Static_assert(UPLOAD_AUTH_SIGNATURE_DER_MAX_BYTES == MBEDTLS_ECDSA_DER_MAX_SIG_LEN(256),
               "UPLOAD_AUTH_SIGNATURE_DER_MAX_BYTES out of sync with mbedtls's own bound");

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

// Like hex_to_bytes(), but accepts any length up to out_cap instead of
// requiring an exact match: a DER-encoded ECDSA signature's length varies
// by a byte or two depending on whether r/s need a leading zero.
static bool hex_to_bytes_var(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (hex == NULL) {
        return false;
    }
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || hex_len / 2 > out_cap) {
        return false;
    }
    size_t n = hex_len / 2;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n;
    return true;
}

// ECDSA P-256 public key used to verify upload signatures, imported once at
// init from the build-time hex-encoded point - only valid when
// upload_auth_enabled() (there's nothing to import otherwise).
static mbedtls_svc_key_id_t s_pubkey;
static bool s_pubkey_valid;

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
        uint8_t pubkey_bytes[UPLOAD_AUTH_PUBKEY_BYTES];
        if (!hex_to_bytes(CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY, pubkey_bytes, sizeof(pubkey_bytes)) ||
            pubkey_bytes[0] != 0x04) {
            ESP_LOGE(TAG,
                     "CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY is not a valid %d-byte hex-encoded "
                     "uncompressed P-256 point",
                     (int)sizeof(pubkey_bytes));
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
            return ESP_ERR_INVALID_ARG;
        }

        psa_status_t status = psa_crypto_init();
        if (status == PSA_SUCCESS) {
            psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
            psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
            psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
            psa_set_key_bits(&attr, 256);
            status = psa_import_key(&attr, pubkey_bytes, sizeof(pubkey_bytes), &s_pubkey);
        }
        if (status != PSA_SUCCESS) {
            // upload_auth_enabled() stays true even on this failure (it's a
            // pure compile-time check) - boot must fail instead of leaving
            // a half-initialized module behind (app_main.cpp's
            // ESP_ERROR_CHECK on this call).
            ESP_LOGE(TAG, "PSA public key import failed (%d)", (int)status);
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
            return ESP_FAIL;
        }
        s_pubkey_valid = true;
    }
    return ESP_OK;
}

void upload_auth_deinit(void) {
    if (s_pubkey_valid) {
        psa_destroy_key(s_pubkey);
        s_pubkey_valid = false;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
}

bool upload_auth_enabled(void) {
    return sizeof(CONFIG_UPLOAD_AUTH_ECDSA_PUBLIC_KEY) > 1;  // more than just the NUL of an empty string
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
    uint8_t der_sig[UPLOAD_AUTH_SIGNATURE_DER_MAX_BYTES];
    size_t der_len;
    if (!hex_to_bytes(nonce_hex, nonce_bytes, sizeof(nonce_bytes)) ||
        !hex_to_bytes_var(signature_hex, der_sig, sizeof(der_sig), &der_len)) {
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

    // openssl (and everything else PKI-shaped) produces ASN.1 DER-encoded
    // ECDSA signatures; PSA only accepts the fixed-width raw r||s form, so
    // convert first - mbedtls's own helper for exactly this conversion.
    uint8_t raw_sig[UPLOAD_AUTH_SIGNATURE_RAW_BYTES];
    size_t raw_len;
    bool sig_ok = mbedtls_ecdsa_der_to_raw(256, der_sig, der_len, raw_sig, sizeof(raw_sig), &raw_len) == 0 &&
                  raw_len == sizeof(raw_sig);
    if (sig_ok) {
        // psa_verify_message() hashes msg itself and checks it against the
        // public key - no separate digest/compare needed on our side.
        psa_status_t status =
            psa_verify_message(s_pubkey, PSA_ALG_ECDSA(PSA_ALG_SHA_256), msg, msg_len, raw_sig, raw_len);
        sig_ok = (status == PSA_SUCCESS);
    }
    free(msg);
    return sig_ok;
}
