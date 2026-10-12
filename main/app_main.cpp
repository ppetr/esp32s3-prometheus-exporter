#include "esp_log.h"

#include "net_eth.h"
#include "metrics_store.h"
#include "channel_registry.h"
#include "lua_pump.h"
#include "http_api.h"
#include "usb_host_bridge.h"
#include "i2c_poll_source.h"

static const char *TAG = "app_main";

// Sequential bring-up. net_eth/http_api/metrics_store are still stubs
// (DESIGN.md §5 has them on Core 0); channel_registry + the lua_pump task
// (Core 1) are real as of this commit, but usb_host_bridge/i2c_poll_source
// not registering any channels yet means the pump just idles - there's
// nothing for it to do until those are wired to
// channel_registry_add_channel().
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "esp32-usb-bridge starting");

    net_eth_init();
    metrics_store_init();
    channel_registry_init();
    http_api_start();
    usb_host_bridge_init();
    i2c_poll_source_init();
    lua_pump_start();

    ESP_LOGI(TAG, "bring-up complete (net_eth/http_api/usb_host_bridge/i2c_poll_source are stubs — see DESIGN.md)");
}
