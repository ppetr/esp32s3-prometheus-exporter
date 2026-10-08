#include "esp_log.h"

#include "net_eth.h"
#include "metrics_store.h"
#include "http_api.h"
#include "usb_host_bridge.h"
#include "i2c_poll_source.h"

static const char *TAG = "app_main";

// Sequential bring-up placeholder. Once components are implemented, this
// becomes task creation with core pinning per DESIGN.md §5 (net_eth/http_api
// on Core 0; usb_host_bridge/lua_sandbox scheduling on Core 1).
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "esp32-usb-bridge starting");

    net_eth_init();
    metrics_store_init();
    http_api_start();
    usb_host_bridge_init();
    i2c_poll_source_init();

    ESP_LOGI(TAG, "bring-up complete (components are stubs — see DESIGN.md)");
}
