#include <string.h>

#include "unity.h"

#include "channel_registry.h"
#include "esp_err.h"

// No setUp/tearDown here - test_metrics_store.c already defines those
// (for metrics_store_init/deinit); each test below calls
// channel_registry_init()/deinit() itself, same as lua_sandbox's tests do
// with lua_sandbox_create()/destroy().

#define TYPICAL_MAX_MEMORY (128 * 1024)

void test_channel_registry_add_and_remove_basic(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_remove_channel("dev0"));
    channel_registry_deinit();
}

void test_channel_registry_add_duplicate_returns_invalid_state(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));
    channel_registry_deinit();
}

void test_channel_registry_add_exceeds_capacity_returns_no_mem(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    char name[CHANNEL_REGISTRY_MAX_NAME_LEN];
    for (int i = 0; i < CHANNEL_REGISTRY_MAX_CHANNELS; i++) {
        snprintf(name, sizeof(name), "dev%d", i);
        TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel(name, TYPICAL_MAX_MEMORY));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, channel_registry_add_channel("one_too_many", TYPICAL_MAX_MEMORY));
    channel_registry_deinit();
}

void test_channel_registry_remove_unknown_returns_not_found(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, channel_registry_remove_channel("nope"));
    channel_registry_deinit();
}

void test_channel_registry_remove_then_readd_same_name(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_remove_channel("dev0"));
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));
    channel_registry_deinit();
}

void test_channel_registry_load_script_unknown_channel_returns_not_found(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, channel_registry_load_script("nope", "return 1", err_buf, sizeof(err_buf)));
    channel_registry_deinit();
}

void test_channel_registry_resume_unknown_channel_returns_not_found(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    uint32_t yield_ms = 0;
    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                       channel_registry_resume("nope", NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    channel_registry_deinit();
}

void test_channel_registry_load_and_resume_end_to_end(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_load_script("dev0", "while true do coroutine.yield(111) end", err_buf,
                                                             sizeof(err_buf)));

    uint32_t yield_ms = 0;
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_resume("dev0", NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(111, yield_ms);

    channel_registry_deinit();
}

// A rejected upload (bad syntax) must not disturb the channel's existing,
// already-running script (DESIGN.md §10: dry-run against the uploaded text
// only, never the live state).
void test_channel_registry_rejected_upload_does_not_clobber_existing_script(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_load_script("dev0", "while true do coroutine.yield(111) end", err_buf,
                                                             sizeof(err_buf)));

    uint32_t yield_ms = 0;
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_resume("dev0", NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(111, yield_ms);

    err_buf[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, channel_registry_load_script("dev0", "if x then", err_buf, sizeof(err_buf)));
    TEST_ASSERT_NOT_EQUAL(0, strlen(err_buf));

    // Old coroutine is still exactly where it left off - still yielding 111.
    yield_ms = 0;
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_resume("dev0", NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(111, yield_ms);

    channel_registry_deinit();
}
