idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_https_ota esp_ringbuf nvs_flash esp_mm esp_driver_i2s driver esp_adc esp_http_server esp_http_client esp_websocket_client
                    INCLUDE_DIRS "."
                    REQUIRES bt)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "$ENV{IDF_PATH}/components/bt/host/nimble/nimble/porting/nimble/include"
)