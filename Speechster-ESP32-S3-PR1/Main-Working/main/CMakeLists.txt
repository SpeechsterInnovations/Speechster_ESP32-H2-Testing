idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_driver_tsens esp_pm esp_wifi esp_https_ota esp_ringbuf nvs_flash esp_mm esp_driver_i2s driver esp_adc esp_http_server esp_http_client esp_websocket_client esp_driver_gpio
                    INCLUDE_DIRS "."
                    REQUIRES bt)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "$ENV{IDF_PATH}/components/bt/host/nimble/nimble/porting/nimble/include"
)

target_compile_definitions(${COMPONENT_TARGET} PRIVATE PROJECT_NAME="${CMAKE_PROJECT_NAME}")

# Extract BUILD version from first line of main.c
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/main.c" VERSION_LINE REGEX "// BUILD:")
string(REGEX REPLACE ".*BUILD:[ \t]*" "" BUILD_VERSION "${VERSION_LINE}")
string(REGEX REPLACE "[ \t\r\n]*$" "" BUILD_VERSION "${BUILD_VERSION}")

# Set build output name using the version from main.c
set(PROJECT_VER "${BUILD_VERSION}")
set(PROJECT_OUTPUT_NAME "${CMAKE_PROJECT_NAME}_${BUILD_VERSION}")

# Tell ESP-IDF to rename final artifacts
set(PROJECT_BIN_NAME "${PROJECT_OUTPUT_NAME}")
set(PROJECT_ELF_NAME "${PROJECT_OUTPUT_NAME}.elf")
set(PROJECT_BIN_NAME "${PROJECT_OUTPUT_NAME}.bin")

message(STATUS "Using firmware version: ${BUILD_VERSION}")
message(STATUS "Output file will be: ${PROJECT_OUTPUT_NAME}.bin")
