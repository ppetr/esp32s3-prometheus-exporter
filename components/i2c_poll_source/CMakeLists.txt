idf_component_register(
    SRCS "i2c_poll_source.c"
    INCLUDE_DIRS "include"
    REQUIRES channel_source
    PRIV_REQUIRES driver
)
