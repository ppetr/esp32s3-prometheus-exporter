#include <string.h>

#include "unity.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "metrics_store.h"
#include "metrics_store_internal.h"

void setUp(void) {
    metrics_store_init();
}

void tearDown(void) {
    metrics_store_deinit();
}

void test_metrics_store_set_before_init_returns_invalid_state(void) {
    metrics_store_deinit();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, metrics_store_set("dev0", "temp", 1.0f));
}

void test_metrics_store_set_null_args_returns_invalid_arg(void) {
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, metrics_store_set(NULL, "temp", 1.0f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, metrics_store_set("dev0", NULL, 1.0f));
}

void test_metrics_store_set_and_format_basic(void) {
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_set("dev0", "temp", 23.5f));

    char buf[256];
    int n = metrics_store_format(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL(n, (int)strlen(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "temp{channel=\"dev0\"} 23.5\n"));
}

void test_metrics_store_channel_status_online_health_metric(void) {
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_set_channel_status("dev0", true, NULL));

    char buf[256];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "usb_bridge_channel_info{channel=\"dev0\",status=\"online\"} 1\n"));
}

void test_metrics_store_channel_status_offline_with_error(void) {
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_set_channel_status("dev0", false, "bad \"frame\""));

    char buf[256];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(
        buf, "usb_bridge_channel_info{channel=\"dev0\",status=\"offline\",lua_error=\"bad \\\"frame\\\"\"} 1\n"));
}

void test_metrics_store_format_omits_stale_metric(void) {
    TickType_t now = xTaskGetTickCount();
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_set("dev0", "temp", 1.0f));

    char buf[256];
    // Still fresh: present.
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format_at(buf, sizeof(buf), now));
    TEST_ASSERT_NOT_NULL(strstr(buf, "temp{channel=\"dev0\"}"));

    // Past the staleness threshold: omitted entirely (never report 0/stale).
    uint32_t stale_now = now + pdMS_TO_TICKS(METRICS_STORE_STALENESS_MS) + 1;
    TEST_ASSERT_GREATER_OR_EQUAL(0, metrics_store_format_at(buf, sizeof(buf), stale_now));
    TEST_ASSERT_NULL(strstr(buf, "temp{channel=\"dev0\"}"));
}

void test_metrics_store_format_buffer_too_small_returns_negative(void) {
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_set("dev0", "temp", 1.0f));

    char buf[4];
    TEST_ASSERT_EQUAL(-1, metrics_store_format(buf, sizeof(buf)));
}

void test_metrics_store_set_exceeds_metric_capacity_returns_no_mem(void) {
    char name[METRICS_STORE_MAX_NAME_LEN];
    for (int i = 0; i < METRICS_STORE_MAX_METRICS_PER_CHANNEL; i++) {
        snprintf(name, sizeof(name), "m%d", i);
        TEST_ASSERT_EQUAL(ESP_OK, metrics_store_set("dev0", name, 1.0f));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, metrics_store_set("dev0", "one_too_many", 1.0f));
}
