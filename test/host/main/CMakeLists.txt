idf_component_register(
    SRCS "test_runner.c" "test_metrics_store.c" "test_lua_sandbox.c"
    INCLUDE_DIRS "."
    REQUIRES metrics_store lua_sandbox unity
)
