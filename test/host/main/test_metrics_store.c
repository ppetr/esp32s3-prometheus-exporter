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

void test_metrics_store_counter_add_accumulates_and_formats(void) {
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_counter_add("dev0", "errors_total", 3.0));
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_counter_add("dev0", "errors_total", 2.0));

    char buf[256];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "errors_total{channel=\"dev0\"} 5\n"));
}

void test_metrics_store_counter_add_negative_returns_invalid_arg(void) {
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_counter_add("dev0", "errors_total", 3.0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, metrics_store_counter_add("dev0", "errors_total", -1.0));
}

void test_metrics_store_counter_add_on_existing_gauge_name_returns_invalid_state(void) {
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_set("dev0", "temp", 1.0f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, metrics_store_counter_add("dev0", "temp", 1.0));
}

void test_metrics_store_set_on_existing_counter_name_returns_invalid_state(void) {
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_counter_add("dev0", "errors_total", 1.0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, metrics_store_set("dev0", "errors_total", 1.0f));
}

void test_metrics_store_set_on_existing_histogram_name_returns_invalid_state(void) {
    float bounds[] = {10.0f};
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_init("dev0", "lat", bounds, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, metrics_store_set("dev0", "lat", 1.0f));
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

void test_metrics_store_histogram_observe_without_init_returns_not_found(void) {
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, metrics_store_histogram_observe("dev0", "lat", 1.0f));
}

void test_metrics_store_histogram_init_rejects_invalid_args(void) {
    float bounds[] = {10.0f, 50.0f};
    float unsorted[] = {50.0f, 10.0f};
    float too_many[METRICS_STORE_MAX_HISTOGRAM_BUCKETS + 1] = {0};
    for (size_t i = 0; i < METRICS_STORE_MAX_HISTOGRAM_BUCKETS + 1; i++) {
        too_many[i] = (float)(i + 1);
    }

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, metrics_store_histogram_init("dev0", "lat", NULL, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, metrics_store_histogram_init("dev0", "lat", bounds, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, metrics_store_histogram_init("dev0", "lat", unsorted, 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                       metrics_store_histogram_init("dev0", "lat", too_many, METRICS_STORE_MAX_HISTOGRAM_BUCKETS + 1));
}

void test_metrics_store_histogram_init_on_existing_gauge_name_returns_invalid_state(void) {
    float bounds[] = {10.0f};
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_set("dev0", "temp", 1.0f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, metrics_store_histogram_init("dev0", "temp", bounds, 1));
}

void test_metrics_store_histogram_observe_and_format(void) {
    float bounds[] = {10.0f, 50.0f};
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_init("dev0", "lat", bounds, 2));

    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_observe("dev0", "lat", 5.0f));    // bucket le=10
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_observe("dev0", "lat", 20.0f));   // bucket le=50
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_observe("dev0", "lat", 100.0f));  // +Inf only

    char buf[512];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "lat_bucket{channel=\"dev0\",le=\"10\"} 1\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "lat_bucket{channel=\"dev0\",le=\"50\"} 2\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "lat_bucket{channel=\"dev0\",le=\"+Inf\"} 3\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "lat_sum{channel=\"dev0\"} 125\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "lat_count{channel=\"dev0\"} 3\n"));
}

void test_metrics_store_histogram_reinit_resets_counts(void) {
    float bounds[] = {10.0f};
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_init("dev0", "lat", bounds, 1));
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_observe("dev0", "lat", 1.0f));

    float new_bounds[] = {5.0f, 15.0f};
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_init("dev0", "lat", new_bounds, 2));

    char buf[256];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "lat_bucket{channel=\"dev0\",le=\"5\"} 0\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "lat_count{channel=\"dev0\"} 0\n"));
}

void test_metrics_store_histogram_omitted_when_stale(void) {
    TickType_t now = xTaskGetTickCount();
    float bounds[] = {10.0f};
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_init("dev0", "lat", bounds, 1));
    TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_observe("dev0", "lat", 1.0f));

    char buf[256];
    TEST_ASSERT_GREATER_THAN(0, metrics_store_format_at(buf, sizeof(buf), now));
    TEST_ASSERT_NOT_NULL(strstr(buf, "lat_count{channel=\"dev0\"}"));

    uint32_t stale_now = now + pdMS_TO_TICKS(METRICS_STORE_STALENESS_MS) + 1;
    TEST_ASSERT_GREATER_OR_EQUAL(0, metrics_store_format_at(buf, sizeof(buf), stale_now));
    TEST_ASSERT_NULL(strstr(buf, "lat_count{channel=\"dev0\"}"));
}

void test_metrics_store_all_metric_slots_can_be_histograms(void) {
    char name[METRICS_STORE_MAX_NAME_LEN];
    float bounds[] = {10.0f};
    for (int i = 0; i < METRICS_STORE_MAX_METRICS_PER_CHANNEL; i++) {
        snprintf(name, sizeof(name), "h%d", i);
        TEST_ASSERT_EQUAL(ESP_OK, metrics_store_histogram_init("dev0", name, bounds, 1));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, metrics_store_histogram_init("dev0", "one_too_many", bounds, 1));
}
