#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>

// Define the ADC unit and channel for your FSR sensor.
// ADC1_CHANNEL_1 corresponds to GPIO1 on the ESP32-H2.
#define ADC_UNIT ADC_UNIT_1
#define ADC_CHANNEL ADC_CHANNEL_1

// Global variable to store the pressure sensor data.
// Note: C does not allow periods in variable names, so we use an underscore.
int p_Sensor_Data_RAW;

// ADC handle.
static adc_oneshot_unit_handle_t adc_handle;

/**
 * @brief Initializes the ADC unit.
 */
static void adc_init(void)
{
    //-------------ADC1 Init-------------//
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
    };
    // Initialize the ADC unit using the configuration.
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    //-------------ADC1 Config-------------//
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12, // Set attenuation to 12 dB for full-range voltage (0V-3.3V).
        .bitwidth = ADC_BITWIDTH_12, // Set resolution to 12 bits (0-4095).
    };
    // Configure the ADC channel.
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL, &config));

    printf("ADC initialized successfully.\n");
}

/**
 * @brief Main application entry point.
 */
void app_main(void)
{
    // Initialize the ADC.
    adc_init();

    printf("Starting FSR402 pressure sensor reading...\n");

    while (1) {
        // Read the raw ADC value from the specified channel.
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL, &p_Sensor_Data_RAW));

        // Print the raw data to the serial monitor.
        printf("FSR402 Raw Data: %d\n", p_Sensor_Data_RAW);

        // Wait for 100 milliseconds before taking the next reading.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
