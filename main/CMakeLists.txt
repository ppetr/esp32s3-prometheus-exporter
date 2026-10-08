idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        channel_source
        usb_host_bridge
        i2c_poll_source
        lua_sandbox
        metrics_store
        http_api
        net_eth
)
