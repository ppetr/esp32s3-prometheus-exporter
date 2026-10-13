#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "esp_err.h"
#include "psa/crypto.h"
#include "upload_auth.h"

// No setUp/tearDown here - test_metrics_store.c already defines those; each
// test below calls upload_auth_init()/deinit() itself, same as
// channel_registry's/lua_sandbox's tests do.

// This build's test/host/sdkconfig.defaults pins CONFIG_UPLOAD_AUTH_HMAC_KEY
// to a non-empty test secret specifically so these tests exercise the
// enabled path; the Kconfig-empty/disabled path is a one-line early return
// in every upload_auth function and isn't separately covered here (same
// carve-out DESIGN.md §12.2 already applies to other one-line passthroughs).

// Mirrors upload_auth.c's own signed-string construction
// ("<channel>,<nonce_hex>,<body>") so tests can produce a correct signature
// without reaching into upload_auth.c's static helpers.
static void sign_hex(const char *channel, const char *nonce_hex, const uint8_t *body, size_t body_len,
                      char *out_sig_hex) {
    size_t channel_len = strlen(channel);
    size_t nonce_len = strlen(nonce_hex);
    size_t msg_len = channel_len + 1 + nonce_len + 1 + body_len;
    uint8_t *msg = malloc(msg_len);
    size_t off = 0;
    memcpy(msg + off, channel, channel_len);
    off += channel_len;
    msg[off++] = ',';
    memcpy(msg + off, nonce_hex, nonce_len);
    off += nonce_len;
    msg[off++] = ',';
    memcpy(msg + off, body, body_len);

    // Independent key import with SIGN_MESSAGE usage - upload_auth's own key
    // (imported in upload_auth_init()) only allows VERIFY_MESSAGE, so tests
    // can't reuse it to compute the expected signature.
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_crypto_init());
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    mbedtls_svc_key_id_t key;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_import_key(&attr, (const uint8_t *)CONFIG_UPLOAD_AUTH_HMAC_KEY,
                                                   strlen(CONFIG_UPLOAD_AUTH_HMAC_KEY), &key));

    uint8_t sig[32];
    size_t sig_len;
    TEST_ASSERT_EQUAL(PSA_SUCCESS,
                       psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, msg_len, sig, sizeof(sig), &sig_len));
    psa_destroy_key(key);
    free(msg);

    for (size_t i = 0; i < sig_len; i++) {
        snprintf(out_sig_hex + 2 * i, 3, "%02x", sig[i]);
    }
}

void test_upload_auth_enabled_when_key_configured(void) {
    TEST_ASSERT_TRUE(upload_auth_enabled());
}

void test_upload_auth_issue_and_verify_succeeds(void) {
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_init());

    char nonce_hex[UPLOAD_AUTH_NONCE_HEX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_issue_nonce(nonce_hex, 0));

    const uint8_t body[] = "room_temp = 21.5";
    char sig_hex[UPLOAD_AUTH_SIGNATURE_HEX_LEN];
    sign_hex("dev0", nonce_hex, body, sizeof(body) - 1, sig_hex);

    TEST_ASSERT_TRUE(upload_auth_verify_and_consume("dev0", nonce_hex, sig_hex, body, sizeof(body) - 1, 0));
    upload_auth_deinit();
}

void test_upload_auth_verify_rejects_wrong_signature(void) {
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_init());

    char nonce_hex[UPLOAD_AUTH_NONCE_HEX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_issue_nonce(nonce_hex, 0));

    const uint8_t body[] = "room_temp = 21.5";
    char sig_hex[UPLOAD_AUTH_SIGNATURE_HEX_LEN];
    sign_hex("dev0", nonce_hex, body, sizeof(body) - 1, sig_hex);
    sig_hex[0] = (sig_hex[0] == '0') ? '1' : '0';  // flip a nibble

    TEST_ASSERT_FALSE(upload_auth_verify_and_consume("dev0", nonce_hex, sig_hex, body, sizeof(body) - 1, 0));
    upload_auth_deinit();
}

void test_upload_auth_verify_rejects_unknown_nonce(void) {
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_init());

    const char *fake_nonce = "00000000000000000000000000000000";
    const uint8_t body[] = "room_temp = 21.5";
    char sig_hex[UPLOAD_AUTH_SIGNATURE_HEX_LEN];
    sign_hex("dev0", fake_nonce, body, sizeof(body) - 1, sig_hex);

    TEST_ASSERT_FALSE(upload_auth_verify_and_consume("dev0", fake_nonce, sig_hex, body, sizeof(body) - 1, 0));
    upload_auth_deinit();
}

void test_upload_auth_nonce_is_single_use(void) {
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_init());

    char nonce_hex[UPLOAD_AUTH_NONCE_HEX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_issue_nonce(nonce_hex, 0));

    const uint8_t body[] = "room_temp = 21.5";
    char sig_hex[UPLOAD_AUTH_SIGNATURE_HEX_LEN];
    sign_hex("dev0", nonce_hex, body, sizeof(body) - 1, sig_hex);

    TEST_ASSERT_TRUE(upload_auth_verify_and_consume("dev0", nonce_hex, sig_hex, body, sizeof(body) - 1, 0));
    // Same signed request replayed - the nonce was already consumed.
    TEST_ASSERT_FALSE(upload_auth_verify_and_consume("dev0", nonce_hex, sig_hex, body, sizeof(body) - 1, 0));
    upload_auth_deinit();
}

void test_upload_auth_verify_rejects_expired_nonce(void) {
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_init());

    char nonce_hex[UPLOAD_AUTH_NONCE_HEX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_issue_nonce(nonce_hex, 0));

    const uint8_t body[] = "room_temp = 21.5";
    char sig_hex[UPLOAD_AUTH_SIGNATURE_HEX_LEN];
    sign_hex("dev0", nonce_hex, body, sizeof(body) - 1, sig_hex);

    // Comfortably past the few-minutes TTL, well under a tick-counter wrap.
    uint32_t far_future_ticks = 10 * 60 * 1000;
    TEST_ASSERT_FALSE(
        upload_auth_verify_and_consume("dev0", nonce_hex, sig_hex, body, sizeof(body) - 1, far_future_ticks));
    upload_auth_deinit();
}

void test_upload_auth_verify_rejects_nonce_for_different_channel(void) {
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_init());

    char nonce_hex[UPLOAD_AUTH_NONCE_HEX_LEN];
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_issue_nonce(nonce_hex, 0));

    const uint8_t body[] = "room_temp = 21.5";
    char sig_hex[UPLOAD_AUTH_SIGNATURE_HEX_LEN];
    sign_hex("dev0", nonce_hex, body, sizeof(body) - 1, sig_hex);  // signed for "dev0"

    // Same nonce+signature, replayed against a different channel.
    TEST_ASSERT_FALSE(upload_auth_verify_and_consume("dev1", nonce_hex, sig_hex, body, sizeof(body) - 1, 0));
    upload_auth_deinit();
}

void test_upload_auth_verify_rejects_malformed_hex(void) {
    TEST_ASSERT_EQUAL(ESP_OK, upload_auth_init());

    const uint8_t body[] = "room_temp = 21.5";
    TEST_ASSERT_FALSE(
        upload_auth_verify_and_consume("dev0", "not-hex", "also-not-hex", body, sizeof(body) - 1, 0));
    upload_auth_deinit();
}
