idf_component_register(
    SRCS "test_runner.c" "test_metrics_store.c" "test_lua_sandbox.c" "test_channel_registry.c" "test_lua_pump.c"
    INCLUDE_DIRS "."
    REQUIRES metrics_store lua_sandbox channel_registry lua_pump unity
)
