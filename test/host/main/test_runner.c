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

extern void test_lua_sandbox_create_destroy_basic(void);
extern void test_lua_sandbox_check_syntax_accepts_valid_source(void);
extern void test_lua_sandbox_check_syntax_rejects_invalid_source(void);
extern void test_lua_sandbox_stripped_globals_are_absent(void);
extern void test_lua_sandbox_os_io_debug_package_not_loaded(void);
extern void test_lua_sandbox_restricted_libraries_are_usable(void);
extern void test_lua_sandbox_resume_without_load_returns_invalid_state(void);
extern void test_lua_sandbox_stream_parser_yields_and_resumes(void);
extern void test_lua_sandbox_poll_parser_yield_ms(void);
extern void test_lua_sandbox_load_replaces_previous_coroutine(void);
extern void test_lua_sandbox_instruction_hook_fires_on_runaway_script(void);
extern void test_lua_sandbox_memory_cap_enforced(void);

extern void test_channel_registry_add_and_remove_basic(void);
extern void test_channel_registry_add_duplicate_returns_invalid_state(void);
extern void test_channel_registry_add_exceeds_capacity_returns_no_mem(void);
extern void test_channel_registry_remove_unknown_returns_not_found(void);
extern void test_channel_registry_remove_then_readd_same_name(void);
extern void test_channel_registry_load_script_unknown_channel_returns_not_found(void);
extern void test_channel_registry_resume_unknown_channel_returns_not_found(void);
extern void test_channel_registry_load_and_resume_end_to_end(void);
extern void test_channel_registry_rejected_upload_does_not_clobber_existing_script(void);
extern void test_channel_registry_pick_due_channel_no_channels_returns_not_found(void);
extern void test_channel_registry_pick_due_channel_fresh_channel_is_immediately_due(void);
extern void test_channel_registry_pick_due_channel_respects_poll_schedule(void);
extern void test_channel_registry_pick_due_channel_picks_most_overdue_among_several(void);
extern void test_channel_registry_recover_stalled_channels_ignores_channel_without_script(void);
extern void test_channel_registry_recover_stalled_channels_ignores_fresh_channels(void);
extern void test_channel_registry_recover_stalled_channels_recreates_coroutine(void);

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
    RUN_TEST(test_lua_sandbox_create_destroy_basic);
    RUN_TEST(test_lua_sandbox_check_syntax_accepts_valid_source);
    RUN_TEST(test_lua_sandbox_check_syntax_rejects_invalid_source);
    RUN_TEST(test_lua_sandbox_stripped_globals_are_absent);
    RUN_TEST(test_lua_sandbox_os_io_debug_package_not_loaded);
    RUN_TEST(test_lua_sandbox_restricted_libraries_are_usable);
    RUN_TEST(test_lua_sandbox_resume_without_load_returns_invalid_state);
    RUN_TEST(test_lua_sandbox_stream_parser_yields_and_resumes);
    RUN_TEST(test_lua_sandbox_poll_parser_yield_ms);
    RUN_TEST(test_lua_sandbox_load_replaces_previous_coroutine);
    RUN_TEST(test_lua_sandbox_instruction_hook_fires_on_runaway_script);
    RUN_TEST(test_lua_sandbox_memory_cap_enforced);
    RUN_TEST(test_channel_registry_add_and_remove_basic);
    RUN_TEST(test_channel_registry_add_duplicate_returns_invalid_state);
    RUN_TEST(test_channel_registry_add_exceeds_capacity_returns_no_mem);
    RUN_TEST(test_channel_registry_remove_unknown_returns_not_found);
    RUN_TEST(test_channel_registry_remove_then_readd_same_name);
    RUN_TEST(test_channel_registry_load_script_unknown_channel_returns_not_found);
    RUN_TEST(test_channel_registry_resume_unknown_channel_returns_not_found);
    RUN_TEST(test_channel_registry_load_and_resume_end_to_end);
    RUN_TEST(test_channel_registry_rejected_upload_does_not_clobber_existing_script);
    RUN_TEST(test_channel_registry_pick_due_channel_no_channels_returns_not_found);
    RUN_TEST(test_channel_registry_pick_due_channel_fresh_channel_is_immediately_due);
    RUN_TEST(test_channel_registry_pick_due_channel_respects_poll_schedule);
    RUN_TEST(test_channel_registry_pick_due_channel_picks_most_overdue_among_several);
    RUN_TEST(test_channel_registry_recover_stalled_channels_ignores_channel_without_script);
    RUN_TEST(test_channel_registry_recover_stalled_channels_ignores_fresh_channels);
    RUN_TEST(test_channel_registry_recover_stalled_channels_recreates_coroutine);
    exit(UNITY_END());
}
