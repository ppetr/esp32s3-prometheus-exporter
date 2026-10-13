/*
 * Copyright 2026 https://github.com/ppetr/esp32s3-prometheus-exporter
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

#include "unity.h"

extern void test_metrics_store_set_before_init_returns_invalid_state(void);
extern void test_metrics_store_set_null_args_returns_invalid_arg(void);
extern void test_metrics_store_format_includes_uptime(void);
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
extern void test_metrics_store_memory_info_omitted_until_set(void);
extern void test_metrics_store_memory_info_included_when_set(void);
extern void test_metrics_store_cpu_usage_included_when_set(void);
extern void test_metrics_store_chip_temp_omitted_until_set(void);
extern void test_metrics_store_chip_temp_included_when_set(void);
extern void test_metrics_store_eth_link_up_includes_speed_and_duplex(void);
extern void test_metrics_store_eth_link_down_omits_speed_and_duplex(void);
extern void test_metrics_store_task_stacks_included_when_set(void);

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
extern void test_lua_sandbox_set_metric_reports_gauge(void);
extern void test_lua_sandbox_set_metric_uses_sandbox_channel(void);
extern void test_lua_sandbox_add_counter_accumulates(void);
extern void test_lua_sandbox_init_and_observe_histogram(void);
extern void test_lua_sandbox_observe_histogram_without_init_raises_lua_error(void);
extern void test_lua_sandbox_memory_cap_enforced(void);
extern void test_lua_sandbox_memory_limit_matches_max_memory_bytes(void);
extern void test_lua_sandbox_memory_used_reflects_allocations(void);
extern void test_lua_sandbox_memory_used_returns_zero_for_null_sandbox(void);

extern void test_channel_registry_add_and_remove_basic(void);
extern void test_channel_registry_add_duplicate_returns_invalid_state(void);
extern void test_channel_registry_add_exceeds_capacity_returns_no_mem(void);
extern void test_channel_registry_remove_unknown_returns_not_found(void);
extern void test_channel_registry_remove_then_readd_same_name(void);
extern void test_channel_registry_load_script_unknown_channel_returns_not_found(void);
extern void test_channel_registry_validate_script_unknown_channel_returns_not_found(void);
extern void test_channel_registry_validate_script_syntax_error_returns_invalid_arg(void);
extern void test_channel_registry_validate_script_does_not_swap_live_script(void);
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
extern void test_channel_registry_no_value_yield_does_not_reschedule_poll(void);
extern void test_channel_registry_push_data_unknown_channel_returns_not_found(void);
extern void test_channel_registry_pick_channel_with_data_returns_zero_when_none_pending(void);
extern void test_channel_registry_push_data_and_pick_channel_with_data(void);
extern void test_channel_registry_push_data_full_buffer_returns_no_mem(void);
extern void test_channel_registry_get_memory_usage_unknown_channel_returns_not_found(void);
extern void test_channel_registry_get_memory_usage_reports_limit(void);

extern void test_lua_pump_step_returns_false_when_nothing_to_do(void);
extern void test_lua_pump_step_drains_pending_data(void);
extern void test_lua_pump_step_resumes_due_poll_channel(void);
extern void test_lua_pump_step_reports_error_via_metrics_store(void);
extern void test_lua_pump_step_reports_lua_memory_usage(void);
extern void test_lua_pump_start_stop_lifecycle(void);

extern void test_upload_auth_enabled_when_key_configured(void);
extern void test_upload_auth_issue_and_verify_succeeds(void);
extern void test_upload_auth_verify_rejects_wrong_signature(void);
extern void test_upload_auth_verify_rejects_unknown_nonce(void);
extern void test_upload_auth_nonce_is_single_use(void);
extern void test_upload_auth_verify_rejects_expired_nonce(void);
extern void test_upload_auth_verify_rejects_nonce_for_different_channel(void);
extern void test_upload_auth_verify_rejects_malformed_hex(void);

// UNITY_END() returns the failure count; exit with it so the Linux-target
// binary terminates instead of idling in the FreeRTOS scheduler (DESIGN.md
// §12.2 relies on this to run as a plain host-native test binary).
void app_main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_metrics_store_set_before_init_returns_invalid_state);
    RUN_TEST(test_metrics_store_set_null_args_returns_invalid_arg);
    RUN_TEST(test_metrics_store_format_includes_uptime);
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
    RUN_TEST(test_metrics_store_memory_info_omitted_until_set);
    RUN_TEST(test_metrics_store_memory_info_included_when_set);
    RUN_TEST(test_metrics_store_cpu_usage_included_when_set);
    RUN_TEST(test_metrics_store_chip_temp_omitted_until_set);
    RUN_TEST(test_metrics_store_chip_temp_included_when_set);
    RUN_TEST(test_metrics_store_eth_link_up_includes_speed_and_duplex);
    RUN_TEST(test_metrics_store_eth_link_down_omits_speed_and_duplex);
    RUN_TEST(test_metrics_store_task_stacks_included_when_set);
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
    RUN_TEST(test_lua_sandbox_set_metric_reports_gauge);
    RUN_TEST(test_lua_sandbox_set_metric_uses_sandbox_channel);
    RUN_TEST(test_lua_sandbox_add_counter_accumulates);
    RUN_TEST(test_lua_sandbox_init_and_observe_histogram);
    RUN_TEST(test_lua_sandbox_observe_histogram_without_init_raises_lua_error);
    RUN_TEST(test_lua_sandbox_memory_cap_enforced);
    RUN_TEST(test_lua_sandbox_memory_limit_matches_max_memory_bytes);
    RUN_TEST(test_lua_sandbox_memory_used_reflects_allocations);
    RUN_TEST(test_lua_sandbox_memory_used_returns_zero_for_null_sandbox);
    RUN_TEST(test_channel_registry_add_and_remove_basic);
    RUN_TEST(test_channel_registry_add_duplicate_returns_invalid_state);
    RUN_TEST(test_channel_registry_add_exceeds_capacity_returns_no_mem);
    RUN_TEST(test_channel_registry_remove_unknown_returns_not_found);
    RUN_TEST(test_channel_registry_remove_then_readd_same_name);
    RUN_TEST(test_channel_registry_load_script_unknown_channel_returns_not_found);
    RUN_TEST(test_channel_registry_validate_script_unknown_channel_returns_not_found);
    RUN_TEST(test_channel_registry_validate_script_syntax_error_returns_invalid_arg);
    RUN_TEST(test_channel_registry_validate_script_does_not_swap_live_script);
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
    RUN_TEST(test_channel_registry_no_value_yield_does_not_reschedule_poll);
    RUN_TEST(test_channel_registry_push_data_unknown_channel_returns_not_found);
    RUN_TEST(test_channel_registry_pick_channel_with_data_returns_zero_when_none_pending);
    RUN_TEST(test_channel_registry_push_data_and_pick_channel_with_data);
    RUN_TEST(test_channel_registry_push_data_full_buffer_returns_no_mem);
    RUN_TEST(test_channel_registry_get_memory_usage_unknown_channel_returns_not_found);
    RUN_TEST(test_channel_registry_get_memory_usage_reports_limit);
    RUN_TEST(test_lua_pump_step_returns_false_when_nothing_to_do);
    RUN_TEST(test_lua_pump_step_drains_pending_data);
    RUN_TEST(test_lua_pump_step_resumes_due_poll_channel);
    RUN_TEST(test_lua_pump_step_reports_error_via_metrics_store);
    RUN_TEST(test_lua_pump_step_reports_lua_memory_usage);
    RUN_TEST(test_lua_pump_start_stop_lifecycle);
    RUN_TEST(test_upload_auth_enabled_when_key_configured);
    RUN_TEST(test_upload_auth_issue_and_verify_succeeds);
    RUN_TEST(test_upload_auth_verify_rejects_wrong_signature);
    RUN_TEST(test_upload_auth_verify_rejects_unknown_nonce);
    RUN_TEST(test_upload_auth_nonce_is_single_use);
    RUN_TEST(test_upload_auth_verify_rejects_expired_nonce);
    RUN_TEST(test_upload_auth_verify_rejects_nonce_for_different_channel);
    RUN_TEST(test_upload_auth_verify_rejects_malformed_hex);
    exit(UNITY_END());
}
