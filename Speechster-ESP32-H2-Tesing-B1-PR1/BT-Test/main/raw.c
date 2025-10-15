#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

#define TAG "I2S_TEST"

/* I2S pins for ESP32-H2 Supermini */
#define I2S_PORT_NUM   I2S_NUM_0
#define I2S_BCK_IO     3
#define I2S_WS_IO      14
#define I2S_DATA_IN_IO 10

#define SAMPLE_BUF_LEN 128

static i2s_chan_handle_t rx_chan = NULL;

void i2s_init_test(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_PORT_NUM,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4,
        .dma_frame_num = 32,
        .auto_clear = true,
    };

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DATA_IN_IO,
        }
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(TAG, "I2S initialized: BCLK=%d WS=%d DIN=%d", I2S_BCK_IO, I2S_WS_IO, I2S_DATA_IN_IO);
}

void i2s_test_task(void *arg)
{
    int32_t i2s_buf[SAMPLE_BUF_LEN];
    int16_t pcm16_buf[SAMPLE_BUF_LEN];
    size_t bytes_read;

    printf("I2S raw data check started...\n");

    while (1) {
        i2s_channel_read(rx_chan, i2s_buf, sizeof(i2s_buf), &bytes_read, portMAX_DELAY);
        int samples = bytes_read / sizeof(int32_t);

        int32_t sum = 0;
        for (int i = 0; i < samples; i++) {
            int32_t sample32 = i2s_buf[i];
            int16_t sample16 = (int16_t)(sample32 >> 8);  // 24-bit -> 16-bit
            pcm16_buf[i] = sample16;
            sum += abs(sample16);
        }

        // Print first few samples and avg amplitude
        printf("Samples: ");
        for (int i = 0; i < 8 && i < samples; i++) {
            printf("%04X ", pcm16_buf[i]);
        }
        printf(" | Avg=%d\n", sum / samples);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    i2s_init_test();
    xTaskCreate(i2s_test_task, "i2s_test", 4096, NULL, 5, NULL);
}
