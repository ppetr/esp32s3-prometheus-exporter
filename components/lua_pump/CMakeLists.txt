idf_component_register(
    SRCS "lua_pump.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES channel_registry metrics_store
)
