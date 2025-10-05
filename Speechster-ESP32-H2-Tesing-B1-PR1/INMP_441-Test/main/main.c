/*
  INMP441_RMS_TEST.c
  Reads audio from INMP441 via I2S and logs RMS volume levels.
  ESP32-H2 friendly — no USB streaming required.

  Wiring (example):
    WS (LRCK) -> GPIO_NUM_5
    BCLK      -> GPIO_NUM_4
    SD (DIN)  -> GPIO_NUM_10

  Adjust GPIO pins below to match your board.
*/

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "INMP441_RMS";

#define SAMPLE_RATE        16000
#define I2S_NUM            (I2S_NUM_0)

/* GPIO pins — update to match your wiring */
#define I2S_BCLK_PIN       GPIO_NUM_4
#define I2S_WS_PIN         GPIO_NUM_5
#define I2S_SD_PIN         GPIO_NUM_10

/* Buffer settings */
#define I2S_READ_SAMPLES   512  // number of 32-bit samples per read

static i2s_chan_handle_t s_rx_handle = NULL;

/* --- I2S Setup --- */
static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = 64;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    ESP_LOGI(TAG, "I2S initialized @ %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

/* --- Task: read samples, compute RMS --- */
static void mic_task(void *arg)
{
    const size_t bytes_to_read = I2S_READ_SAMPLES * sizeof(int32_t);
    int32_t *i2s_buf = heap_caps_malloc(bytes_to_read, MALLOC_CAP_DEFAULT);
    if (!i2s_buf) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        vTaskDelete(NULL);
    }

    while (1) {
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(s_rx_handle, i2s_buf, bytes_to_read, &bytes_read, pdMS_TO_TICKS(200));
        if (r == ESP_OK && bytes_read > 0) {
            size_t frames = bytes_read / sizeof(int32_t);
            double sum_sq = 0.0;

            for (size_t i = 0; i < frames; i++) {
                int32_t s32 = i2s_buf[i];
                // INMP441 output: 24-bit left-aligned in 32-bit slot
                int16_t s16 = (int16_t)(s32 >> 14);
                sum_sq += (double)s16 * (double)s16;
            }

            double rms = sqrt(sum_sq / frames);
            ESP_LOGI(TAG, "Audio RMS = %.2f", rms);
        } else {
            ESP_LOGW(TAG, "No I2S data read");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    heap_caps_free(i2s_buf);
    vTaskDelete(NULL);
}

/* --- Main entry --- */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting INMP441 RMS audio test...");

    if (i2s_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S");
        return;
    }

    xTaskCreatePinnedToCore(mic_task, "mic_task", 4096, NULL,
                            tskIDLE_PRIORITY + 2, NULL, 0);

    ESP_LOGI(TAG, "Mic task started!");
}
