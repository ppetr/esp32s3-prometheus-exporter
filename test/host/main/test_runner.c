#include "unity.h"

extern void test_metrics_store_init_returns_not_supported(void);

void app_main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_metrics_store_init_returns_not_supported);
    UNITY_END();
}
