cmake_minimum_required(VERSION 3.5)

# This file tells CMake which source files to compile for the 'main' component.
idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES nvs_flash driver freertos esp_timer
    INCLUDE_DIRS "."
    REQUIRES bt 
)



# --- Manual include patch for ESP-IDF NimBLE paths ---
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${IDF_PATH}/components/bt/host/nimble/nimble/porting/nimble/include"
    "${IDF_PATH}/components/bt/host/nimble/esp-hci/include"
)
