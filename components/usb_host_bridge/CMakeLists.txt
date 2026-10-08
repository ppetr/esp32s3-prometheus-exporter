idf_component_register(
    SRCS "usb_host_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES channel_source
    PRIV_REQUIRES usb
)
