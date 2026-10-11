#include <stdlib.h>

#include "unity.h"

extern void test_metrics_store_set_before_init_returns_invalid_state(void);
extern void test_metrics_store_set_null_args_returns_invalid_arg(void);
extern void test_metrics_store_set_and_format_basic(void);
extern void test_metrics_store_counter_add_accumulates_and_formats(void);
extern void test_metrics_store_counter_add_negative_returns_invalid_arg(void);
extern void test_metrics_store_counter_add_on_existing_gauge_name_returns_invalid_state(void);
extern void test_metrics_store_set_on_existing_counter_name_returns_invalid_state(void);
extern void test_metrics_store_set_on_existing_histogram_name_returns_invalid_state(void);
extern void test_metrics_store_channel_status_online_health_metric(void);
extern void test_metrics_store_channel_status_offline_with_error(void);
extern void test_metrics_store_format_omits_stale_metric(void);
extern void test_metrics_store_format_buffer_too_small_returns_negative(void);
extern void test_metrics_store_set_exceeds_metric_capacity_returns_no_mem(void);
extern void test_metrics_store_histogram_observe_without_init_returns_not_found(void);
extern void test_metrics_store_histogram_init_rejects_invalid_args(void);
extern void test_metrics_store_histogram_init_on_existing_gauge_name_returns_invalid_state(void);
extern void test_metrics_store_histogram_observe_and_format(void);
extern void test_metrics_store_histogram_reinit_resets_counts(void);
extern void test_metrics_store_histogram_omitted_when_stale(void);
extern void test_metrics_store_all_metric_slots_can_be_histograms(void);

// UNITY_END() returns the failure count; exit with it so the Linux-target
// binary terminates instead of idling in the FreeRTOS scheduler (DESIGN.md
// §12.2 relies on this to run as a plain host-native test binary).
void app_main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_metrics_store_set_before_init_returns_invalid_state);
    RUN_TEST(test_metrics_store_set_null_args_returns_invalid_arg);
    RUN_TEST(test_metrics_store_set_and_format_basic);
    RUN_TEST(test_metrics_store_counter_add_accumulates_and_formats);
    RUN_TEST(test_metrics_store_counter_add_negative_returns_invalid_arg);
    RUN_TEST(test_metrics_store_counter_add_on_existing_gauge_name_returns_invalid_state);
    RUN_TEST(test_metrics_store_set_on_existing_counter_name_returns_invalid_state);
    RUN_TEST(test_metrics_store_set_on_existing_histogram_name_returns_invalid_state);
    RUN_TEST(test_metrics_store_channel_status_online_health_metric);
    RUN_TEST(test_metrics_store_channel_status_offline_with_error);
    RUN_TEST(test_metrics_store_format_omits_stale_metric);
    RUN_TEST(test_metrics_store_format_buffer_too_small_returns_negative);
    RUN_TEST(test_metrics_store_set_exceeds_metric_capacity_returns_no_mem);
    RUN_TEST(test_metrics_store_histogram_observe_without_init_returns_not_found);
    RUN_TEST(test_metrics_store_histogram_init_rejects_invalid_args);
    RUN_TEST(test_metrics_store_histogram_init_on_existing_gauge_name_returns_invalid_state);
    RUN_TEST(test_metrics_store_histogram_observe_and_format);
    RUN_TEST(test_metrics_store_histogram_reinit_resets_counts);
    RUN_TEST(test_metrics_store_histogram_omitted_when_stale);
    RUN_TEST(test_metrics_store_all_metric_slots_can_be_histograms);
    exit(UNITY_END());
}
