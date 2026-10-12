#include <string.h>

#include "unity.h"

#include "channel_registry.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_pump.h"
#include "metrics_store.h"

// No setUp/tearDown here either - see test_channel_registry.c. Each test
// init/deinits channel_registry itself; metrics_store is already
// init/deinit'd by test_metrics_store.c's global setUp()/tearDown()
// around every test in this binary, lua_pump's included.

#define TYPICAL_MAX_MEMORY (128 * 1024)

void test_lua_pump_step_returns_false_when_nothing_to_do(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());

    TEST_ASSERT_FALSE(lua_pump_step((uint32_t)xTaskGetTickCount(), LUA_PUMP_INSTRUCTION_LIMIT));

    channel_registry_deinit();
}

// Proves the data really flows pick_channel_with_data -> resume: the
// script asserts on the exact bytes pushed, so a wrong/missing drain
// would surface as a reported lua_error instead of a clean "online".
void test_lua_pump_step_drains_pending_data(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    const char *script = "local x = ...\nassert(x == \"42\", \"wrong data: \" .. tostring(x))";
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_load_script("dev0", script, err_buf, sizeof(err_buf)));
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_push_data("dev0", (const uint8_t *)"42", 2));

    TEST_ASSERT_TRUE(lua_pump_step((uint32_t)xTaskGetTickCount(), LUA_PUMP_INSTRUCTION_LIMIT));

    char metrics_buf[256];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format(metrics_buf, sizeof(metrics_buf)));
    TEST_ASSERT_NOT_NULL(strstr(metrics_buf, "usb_bridge_channel_info{channel=\"dev0\",status=\"online\"} 1"));

    channel_registry_deinit();
}

void test_lua_pump_step_resumes_due_poll_channel(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                       channel_registry_load_script("dev0", "room_temp = 21.5", err_buf, sizeof(err_buf)));

    TEST_ASSERT_TRUE(lua_pump_step((uint32_t)xTaskGetTickCount(), LUA_PUMP_INSTRUCTION_LIMIT));

    char metrics_buf[256];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format(metrics_buf, sizeof(metrics_buf)));
    TEST_ASSERT_NOT_NULL(strstr(metrics_buf, "usb_bridge_channel_info{channel=\"dev0\",status=\"online\"} 1"));

    channel_registry_deinit();
}

void test_lua_pump_step_reports_error_via_metrics_store(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_load_script("dev0", "error('boom')", err_buf, sizeof(err_buf)));

    TEST_ASSERT_TRUE(lua_pump_step((uint32_t)xTaskGetTickCount(), LUA_PUMP_INSTRUCTION_LIMIT));

    char metrics_buf[256];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format(metrics_buf, sizeof(metrics_buf)));
    TEST_ASSERT_NOT_NULL(strstr(metrics_buf, "lua_error="));
    TEST_ASSERT_NOT_NULL(strstr(metrics_buf, "boom"));

    channel_registry_deinit();
}

// Lifecycle sanity only (DESIGN.md §5's real task, Core 1 pinned): the
// actual scheduling decisions are lua_pump_step()'s job, already covered
// above without needing a real task. This just proves the task can start,
// run for a bit, and stop cleanly - ASan would flag any crash or leak.
void test_lua_pump_start_stop_lifecycle(void) {
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_init());
    TEST_ASSERT_EQUAL(ESP_OK, channel_registry_add_channel("dev0", TYPICAL_MAX_MEMORY));

    char err_buf[128] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
                       channel_registry_load_script("dev0", "while true do coroutine.yield(5) end", err_buf,
                                                      sizeof(err_buf)));

    TEST_ASSERT_EQUAL(ESP_OK, lua_pump_start());
    vTaskDelay(pdMS_TO_TICKS(50));
    lua_pump_stop();

    channel_registry_deinit();
}
