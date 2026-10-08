idf_component_register(
    SRCS "test_runner.c" "test_metrics_store.c"
    INCLUDE_DIRS "."
    REQUIRES metrics_store unity
)
