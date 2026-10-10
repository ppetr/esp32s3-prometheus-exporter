#include <stdlib.h>

#include "unity.h"

extern void test_metrics_store_init_returns_not_supported(void);

// UNITY_END() returns the failure count; exit with it so the Linux-target
// binary terminates instead of idling in the FreeRTOS scheduler (DESIGN.md
// §12.2 relies on this to run as a plain host-native test binary).
void app_main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_metrics_store_init_returns_not_supported);
    exit(UNITY_END());
}
