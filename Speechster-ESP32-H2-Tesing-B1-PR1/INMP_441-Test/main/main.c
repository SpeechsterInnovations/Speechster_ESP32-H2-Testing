#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_vfs_dev.h"

static const char *TAG = "INMP441_TEST";

// --- Adjust for your board connections ---
#define I2S_WS      3   // LRCL
#define I2S_SCK     2   // BCLK
#define I2S_SD      4   // DOUT from mic

// --- I2S Configuration ---
static i2s_chan_handle_t rx_handle;

void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),  // 16 kHz sample rate
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK,
            .ws   = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S initialized successfully");
}

void app_main(void)
{
    // Use UART0 for stdio
    esp_vfs_dev_uart_use_driver(0);
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "Boot OK - UART logging active");
    vTaskDelay(pdMS_TO_TICKS(500));

    // Initialize I2S
    i2s_init();

    // Basic loop to show firmware is alive
    while (true) {
        ESP_LOGI(TAG, "Main loop alive");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
