idf_component_register(
    SRCS "upload_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls
    PRIV_REQUIRES esp_hw_support
)
