#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_log.h"

#define TAG "INMP441_UART"

// ==== Audio config ====
#define SAMPLE_RATE     16000
#define BITS_PER_SAMPLE 16
#define CHANNELS        1
#define RECORD_TIME_SEC 5
#define BUFFER_SAMPLES  512

// ==== UART config ====
#define UART_PORT   UART_NUM_0
#define UART_BAUD   115200

// ==== WAV header size ====
#define WAV_HEADER_SIZE 44

static i2s_chan_handle_t rx_handle;

// ====== WAV HEADER CREATION ======
static void wav_header(uint8_t *header, uint32_t data_size) {
    uint32_t file_size = data_size + WAV_HEADER_SIZE - 8;
    uint16_t audio_format = 1; // PCM
    uint16_t num_channels = CHANNELS;
    uint16_t bits_per_sample = BITS_PER_SAMPLE;
    uint16_t block_align = CHANNELS * BITS_PER_SAMPLE / 8;
    uint32_t byte_rate = SAMPLE_RATE * block_align;
    uint32_t sample_rate = SAMPLE_RATE;

    memcpy(header, "RIFF", 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);

    uint32_t fmt_chunk_size = 16;
    memcpy(header + 16, &fmt_chunk_size, 4);
    memcpy(header + 20, &audio_format, 2);
    memcpy(header + 22, &num_channels, 2);
    memcpy(header + 24, &sample_rate, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &block_align, 2);
    memcpy(header + 34, &bits_per_sample, 2);

    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &data_size, 4);

    memcpy(header + 4, &file_size, 4);
}

// ====== INIT UART ======
static void uart_init(void) {
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// ====== INIT I2S ======
static void i2s_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_slot_config_t slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_mode      = I2S_SLOT_MODE_MONO,
        .ws_width       = 16,
        .ws_pol         = false,
        .bit_shift      = true,
        .left_align     = true,          // keep standard I2S alignment
        .big_endian     = false,
        .slot_mask      = I2S_STD_SLOT_LEFT  // or RIGHT depending on mic wiring
    };


    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 7,   // BCLK pin
            .ws   = 8,   // LRCLK pin
            .dout = I2S_GPIO_UNUSED,
            .din  = 6    // DIN from INMP441
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

// ====== MAIN TASK ======
void app_main(void) {
    uart_init();
    i2s_init();

    ESP_LOGI(TAG, "Starting 5 sec recording...");

    size_t total_samples = SAMPLE_RATE * RECORD_TIME_SEC;
    size_t total_bytes   = total_samples * (BITS_PER_SAMPLE / 8);

    // Allocate buffer
    int16_t *audio_buf = heap_caps_malloc(total_samples * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return;
    }

    size_t bytes_read = 0, idx = 0;
    int16_t temp_buf[BUFFER_SAMPLES];

    while (idx < total_samples) {
        i2s_channel_read(rx_handle, temp_buf, sizeof(temp_buf), &bytes_read, portMAX_DELAY);
        size_t samples_read = bytes_read / sizeof(int16_t);

        for (size_t i = 0; i < samples_read && idx < total_samples; i++) {
            audio_buf[idx++] = temp_buf[i];
        }
    }

    ESP_LOGI(TAG, "Recording finished, sending WAV...");

    // Send framing header
    uint8_t frame_header[2] = {0xAA, 0x55};
    uart_write_bytes(UART_PORT, (const char *)frame_header, 2);

    // Send WAV header
    uint8_t header[WAV_HEADER_SIZE];
    wav_header(header, total_bytes);
    uart_write_bytes(UART_PORT, (const char *)header, WAV_HEADER_SIZE);

    // Send audio data
    uart_write_bytes(UART_PORT, (const char *)audio_buf, total_bytes);

    ESP_LOGI(TAG, "WAV file sent over UART");

    free(audio_buf);
}
