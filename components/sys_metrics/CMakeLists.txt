idf_component_register(
    SRCS "sys_metrics.c"
    INCLUDE_DIRS "include"
    REQUIRES metrics_store
    PRIV_REQUIRES heap esp_driver_tsens
)
