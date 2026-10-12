#include <string.h>

#include "unity.h"

#include "channel_registry.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

void test_channel_registry_pick_due_channel_no_channels_returns_not_found(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());

    char out[CHANNEL_REGISTRY_MAX_NAME_LEN];
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, channel_registry_pick_due_channel(xTaskGetTickCount(), out, sizeof(out), NULL));

    channel_registry_deinit();
}

void test_channel_registry_pick_due_channel_fresh_channel_is_immediately_due(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char out[CHANNEL_REGISTRY_MAX_NAME_LEN] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_pick_due_channel(xTaskGetTickCount(), out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING("dev0", out);

    channel_registry_deinit();
}

void test_channel_registry_pick_due_channel_respects_poll_schedule(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_load_script("dev0", "while true do coroutine.yield(5000) end", err_buf,
                                                             sizeof(err_buf)));

    TickType_t resume_tick = xTaskGetTickCount();
    uint32_t yield_ms = 0;
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_resume("dev0", NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(5000, yield_ms);

    char out[CHANNEL_REGISTRY_MAX_NAME_LEN] = {0};
    uint32_t wait_ticks = 0;
    // Only 1s elapsed of the 5s requested delay: not due yet.
    esp_err_t err = channel_registry_pick_due_channel(resume_tick + pdMS_TO_TICKS(1000), out, sizeof(out), &wait_ticks);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, err);
    TEST_ASSERT_GREATER_THAN(0, wait_ticks);

    // Past the full 5s delay: due now.
    err = channel_registry_pick_due_channel(resume_tick + pdMS_TO_TICKS(6000), out, sizeof(out), &wait_ticks);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL_STRING("dev0", out);

    channel_registry_deinit();
}

void test_channel_registry_pick_due_channel_picks_most_overdue_among_several(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("a", TYPICAL_MAX_MEMORY));
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("b", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    uint32_t yield_ms = 0;
    TickType_t now = xTaskGetTickCount();

    TEST_ASSERT_EQUAL(ESP_OK,
                       channel_registry_load_script("a", "while true do coroutine.yield(1000) end", err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_resume("a", NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));

    TEST_ASSERT_EQUAL(ESP_OK,
                       channel_registry_load_script("b", "while true do coroutine.yield(500) end", err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_resume("b", NULL, 0, 10000, &yield_ms, err_buf, sizeof(err_buf)));

    // Both are overdue by now; "b" (500ms deadline) is more overdue than "a" (1000ms).
    char out[CHANNEL_REGISTRY_MAX_NAME_LEN] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_pick_due_channel(now + pdMS_TO_TICKS(2000), out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING("b", out);

    channel_registry_deinit();
}

void test_channel_registry_recover_stalled_channels_ignores_channel_without_script(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    TEST_ASSERT_EQUAL(0, channel_registry_recover_stalled_channels(xTaskGetTickCount() + pdMS_TO_TICKS(1000000), 10));

    channel_registry_deinit();
}

void test_channel_registry_recover_stalled_channels_ignores_fresh_channels(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                       channel_registry_load_script("dev0", "coroutine.yield()", err_buf, sizeof(err_buf)));

    // No time has passed since the load - not stalled.
    TEST_ASSERT_EQUAL(0, channel_registry_recover_stalled_channels(xTaskGetTickCount(), pdMS_TO_TICKS(60000)));

    channel_registry_deinit();
}

// Proves the recovered coroutine actually restarted from scratch (DESIGN.md
// §9.2): a script that only reads its first chunk via `...` on its very
// first resume, then ignores further resume arguments. If recovery didn't
// recreate the coroutine, resuming again would continue the old one
// (ignoring the new data); if it did, the new data is read as a fresh `...`.
void test_channel_registry_recover_stalled_channels_recreates_coroutine(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    const char *script = "local x = tonumber(...)\nwhile true do\n  coroutine.yield(x)\n  x = x + 1\nend";
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_load_script("dev0", script, err_buf, sizeof(err_buf)));

    TickType_t resume_tick = xTaskGetTickCount();
    uint32_t yield_ms = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
                       channel_registry_resume("dev0", (const uint8_t *)"10", 2, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(10, yield_ms);

    TEST_ASSERT_EQUAL(
        1, channel_registry_recover_stalled_channels(resume_tick + pdMS_TO_TICKS(60000), pdMS_TO_TICKS(1000)));

    // A continued (not recreated) coroutine would ignore this and yield 11.
    TEST_ASSERT_EQUAL(ESP_OK,
                       channel_registry_resume("dev0", (const uint8_t *)"99", 2, 10000, &yield_ms, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(99, yield_ms);

    channel_registry_deinit();
}
