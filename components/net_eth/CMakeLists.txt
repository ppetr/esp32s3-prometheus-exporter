idf_component_register(
    SRCS "net_eth.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_eth esp_netif lwip
)
