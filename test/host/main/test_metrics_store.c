#include "unity.h"

#include "esp_err.h"
#include "metrics_store.h"

// Placeholder proving the host-test plumbing works end to end. Replace with
// real coverage (staleness omission, health metric, text formatting) once
// metrics_store is implemented — see what belongs here per DESIGN.md §12.2.
void test_metrics_store_init_returns_not_supported(void) {
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, metrics_store_init());
}
