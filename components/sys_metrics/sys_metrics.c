#include "sys_metrics.h"

#include <stdlib.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "metrics_store.h"

static const char *TAG = "sys_metrics";

// Bound on live task count this snapshot can cover; comfortably above what
// this firmware actually creates (DESIGN.md §5). uxTaskGetSystemState()
// truncates rather than overflowing if there are more.
#define SYS_METRICS_MAX_TASKS 32

static bool s_have_prev_cpu_sample;
static uint32_t s_prev_total_runtime;
static uint32_t s_prev_idle_runtime0, s_prev_idle_runtime1;

static void refresh_memory_metrics(void) {
    metrics_store_set_memory_info(heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                   heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                                   heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                                   heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

// uxTaskGetSystemState() is a debug facility that briefly suspends the
// scheduler to snapshot every task - fine once per HTTP scrape, not a hot
// path. Covers both CPU usage (IDLE0/IDLE1's share of the elapsed window)
// and per-task stack headroom (DESIGN.md §7) in the same pass, since both
// come out of the same snapshot. StackType_t is uint8_t on this port, so
// usStackHighWaterMark is already in bytes, not words.
static void refresh_cpu_and_stack_metrics(void) {
    TaskStatus_t *tasks = malloc(SYS_METRICS_MAX_TASKS * sizeof(TaskStatus_t));
    if (tasks == NULL) {
        return;
    }

    uint32_t total_runtime;
    UBaseType_t count = uxTaskGetSystemState(tasks, SYS_METRICS_MAX_TASKS, &total_runtime);

    uint32_t idle_runtime0 = 0, idle_runtime1 = 0;
    const char *names[SYS_METRICS_MAX_TASKS];
    uint32_t stack_free_bytes[SYS_METRICS_MAX_TASKS];
    for (UBaseType_t i = 0; i < count; i++) {
        names[i] = tasks[i].pcTaskName;
        stack_free_bytes[i] = (uint32_t)tasks[i].usStackHighWaterMark;
        if (strcmp(tasks[i].pcTaskName, "IDLE0") == 0) {
            idle_runtime0 = tasks[i].ulRunTimeCounter;
        } else if (strcmp(tasks[i].pcTaskName, "IDLE1") == 0) {
            idle_runtime1 = tasks[i].ulRunTimeCounter;
        }
    }
    metrics_store_set_task_stacks(names, stack_free_bytes, count);

    // Unsigned subtraction wraps correctly across the run-time counter
    // rolling over, same as metrics_store's own tick handling.
    uint32_t total_delta = total_runtime - s_prev_total_runtime;
    if (s_have_prev_cpu_sample && total_delta > 0) {
        uint32_t idle_delta0 = idle_runtime0 - s_prev_idle_runtime0;
        uint32_t idle_delta1 = idle_runtime1 - s_prev_idle_runtime1;
        float usage0 = 100.0f * (1.0f - (float)idle_delta0 / (float)total_delta);
        float usage1 = 100.0f * (1.0f - (float)idle_delta1 / (float)total_delta);
        metrics_store_set_cpu_usage(usage0, usage1);
    }

    s_prev_total_runtime = total_runtime;
    s_prev_idle_runtime0 = idle_runtime0;
    s_prev_idle_runtime1 = idle_runtime1;
    s_have_prev_cpu_sample = true;

    free(tasks);
}

// -10..80 degC range (DESIGN.md §7) - matches ESP-IDF's own
// temperature_sensor example and lands on the chip's most accurate
// calibrated range (+-1 degC), comfortably covering realistic die
// temperatures for this board (no RF, no heavy compute).
static temperature_sensor_handle_t s_tsens;
static bool s_tsens_failed;  // install/enable already tried and failed - don't retry every scrape

static void refresh_chip_temp(void) {
    if (s_tsens == NULL && !s_tsens_failed) {
        temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        temperature_sensor_handle_t tsens = NULL;
        esp_err_t err = temperature_sensor_install(&config, &tsens);
        if (err == ESP_OK) {
            err = temperature_sensor_enable(tsens);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "chip temperature sensor unavailable: %s", esp_err_to_name(err));
            if (tsens != NULL) {
                temperature_sensor_uninstall(tsens);
            }
            s_tsens_failed = true;
            return;
        }
        s_tsens = tsens;
    }
    if (s_tsens == NULL) {
        return;
    }

    float celsius;
    if (temperature_sensor_get_celsius(s_tsens, &celsius) == ESP_OK) {
        metrics_store_set_chip_temp(celsius);
    }
}

void sys_metrics_refresh(void) {
    refresh_memory_metrics();
    refresh_cpu_and_stack_metrics();
    refresh_chip_temp();
}
