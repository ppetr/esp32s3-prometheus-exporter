#include "net_eth.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "metrics_store.h"

static const char *TAG = "net_eth";

static bool s_initialized;
static bool s_time_synchronized;

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED: {
            uint8_t mac_addr[6] = {0};
            esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
            ESP_LOGI(TAG, "link up, MAC " MACSTR, MAC2STR(mac_addr));

            eth_speed_t speed = ETH_SPEED_10M;
            eth_duplex_t duplex = ETH_DUPLEX_HALF;
            esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed);
            esp_eth_ioctl(eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
            int speed_mbps = (speed == ETH_SPEED_1000M) ? 1000 : (speed == ETH_SPEED_100M) ? 100 : 10;
            metrics_store_set_eth_link(true, speed_mbps, duplex == ETH_DUPLEX_FULL);
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "link down");
            metrics_store_set_eth_link(false, 0, false);
            break;
        default:
            break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    (void)event_id;
    const esp_netif_ip_info_t *ip_info = &((ip_event_got_ip_t *)event_data)->ip_info;
    ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ip_info->ip));
}

static void time_sync_notification_cb(struct timeval *tv) {
    (void)tv;
    s_time_synchronized = true;
    ESP_LOGI(TAG, "SNTP time synchronized");
}

esp_err_t net_eth_init(void) {
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_NET_ETH_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_NET_ETH_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_NET_ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)CONFIG_NET_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_NET_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = CONFIG_NET_ETH_SPI_CS_GPIO,
    };

    eth_w5500_config_t w5500_config =
        ETH_W5500_DEFAULT_CONFIG((spi_host_device_t)CONFIG_NET_ETH_SPI_HOST, &devcfg);
    w5500_config.base.int_gpio_num = CONFIG_NET_ETH_INT_GPIO;
    if (CONFIG_NET_ETH_INT_GPIO < 0) {
        w5500_config.base.poll_period_ms = 10;
    } else {
        // gpio_isr_handler_add() (used internally to watch the INT pin)
        // requires this global service to be installed first.
        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500 failed");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = CONFIG_NET_ETH_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "esp_eth_phy_new_w5500 failed");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed: %s", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        return err;
    }

    // W5500 has no factory-burned MAC address; derive one from the S3's
    // own base MAC (DESIGN.md §2).
    uint8_t mac_addr[6];
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_config);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    err = esp_eth_start(eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed: %s", esp_err_to_name(err));
        return err;
    }

    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_NET_ETH_NTP_SERVER);
    esp_sntp_init();

    s_initialized = true;
    return ESP_OK;
}

bool net_eth_time_synchronized(void) {
    return s_time_synchronized;
}
