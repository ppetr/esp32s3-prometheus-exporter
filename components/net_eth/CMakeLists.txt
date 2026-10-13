idf_component_register(
    SRCS "net_eth.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_eth esp_netif lwip esp_event esp_driver_spi esp_driver_gpio w5500 metrics_store
)
