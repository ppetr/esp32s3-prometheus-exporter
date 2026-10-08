idf_component_register(
    SRCS "http_api.c"
    INCLUDE_DIRS "include"
    REQUIRES metrics_store
    PRIV_REQUIRES esp_http_server
)
