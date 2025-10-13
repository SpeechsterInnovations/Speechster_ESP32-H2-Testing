// main.c
// ESP-IDF v5.5.1 (dirty) - NimBLE GATT server with JSON control + stream notify
// INMP441 I2S pins: SD=GPIO10, SCK=GPIO4, WS=GPIO5 (L/R tied to GND -> left channel)
// FSR402 ADC: ADC1 channel 1
// UART logging @115200, advertise automatically.
// ADPCM encoder included (ima adpcm) and selectable at runtime via JSON control.
// cJSON is used for control parsing (available in ESP-IDF).

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include <math.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_timer.h"

/* --- Suppress deprecation warnings for legacy driver headers */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#pragma GCC diagnostic pop

/* NimBLE / BLE includes */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_att.h"

/* JSON parsing */
#include "cJSON.h"

/* Mic I2S STD Driver */
#include "driver/i2s_std.h"
#include "driver/i2s_common.h" 

/* ADPCM header (drop-in) */
#include "adpcm.h"

static const char *TAG = "speechster_h2";

/* ---------- Defaults & config ---------- */
#define UART_BAUD_RATE         115200
#define AUDIO_SAMPLE_RATE      16000
#define AUDIO_FRAME_MS         20
#define AUDIO_FRAME_SAMPLES    ((AUDIO_SAMPLE_RATE/1000) * AUDIO_FRAME_MS) // 320
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_READ_BYTES       (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_RINGBUF_SIZE     (64 * 1024)
#define USE_MIC_TEST_TASK      1

#define I2S_PORT_NUM         I2S_NUM_0
#define I2S_BCK_IO           3
#define I2S_WS_IO            14
#define I2S_DATA_IN_IO       10
#define SAMPLE_BUF_LEN       AUDIO_FRAME_SAMPLES  // unified buffer size

#define FSR_ADC_CHANNEL           ADC_CHANNEL_1
static uint32_t fsr_interval_ms = 50; // default 50ms, can be changed via JSON
adpcm_state_t adpcm_state;

/* BLE custom UUIDs */
static const char *SERVICE_UUID_STR      = "0000feed-0000-1000-8000-00805f9b34fb";
static const char *CHAR_CONTROL_UUID_STR = "0000feed-0001-0000-8000-00805f9b34fb";
static const char *CHAR_STREAM_UUID_STR  = "0000feed-0002-0000-8000-00805f9b34fb";

/* Packet framing (8 byte header) */
static inline void pack_header(uint8_t *buf, uint8_t type, uint8_t flags, uint16_t seq, uint32_t ts_ms) {
    buf[0] = type;
    buf[1] = flags;
    buf[2] = seq & 0xff;
    buf[3] = (seq >> 8) & 0xff;
    buf[4] = ts_ms & 0xff;
    buf[5] = (ts_ms >> 8) & 0xff;
    buf[6] = (ts_ms >> 16) & 0xff;
    buf[7] = (ts_ms >> 24) & 0xff;
}

/* runtime state */
typedef enum { ENC_PCM = 0, ENC_ADPCM = 1 } encoder_t;
static encoder_t default_encoder = ENC_PCM;
static encoder_t current_encoder = ENC_PCM;
static volatile bool mic_enabled = false;
static volatile bool fsr_enabled = false;
static uint16_t seq_counter = 0;

static RingbufHandle_t audio_rb = NULL;
static SemaphoreHandle_t state_lock = NULL;

/* BLE state */
static uint16_t g_conn_handle = 0;
static uint16_t g_stream_attr_handle = 0;
static volatile bool fake_mode = false;

/* --- I2S Channel Handle --- */
static i2s_chan_handle_t rx_chan = NULL; // Handle for the I2S receive channel

/* ---------- I2S (INMP441) ---------- */
static void i2s_init_inmp441(void) {
    // 1. Channel Configuration (including DMA and role)
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,          // ESP32 is clock master (generates BCLK/WS)
        .dma_desc_num = 8,                // Number of DMA descriptors
        .dma_frame_num = AUDIO_FRAME_SAMPLES, // Frames per descriptor
        .auto_clear = true,               // Clear old data when buffer is full
    };
    
    // Create the channel handle (NULL for TX handle implies RX channel).
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    // 2. Standard I2S Mode Configuration
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        // INMP441 data is read as 32-bit (MSB-aligned) on the ESP32.
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        
        // Use the nested structure AND the new, shortened member names:
        .gpio_cfg = {
            .bclk = I2S_BCK_IO,       // Changed from .bclk_io_num
            .ws = I2S_WS_IO,          // Changed from .ws_io_num
            .dout = I2S_GPIO_UNUSED, // Changed from .data_out_num
            .din = I2S_DATA_IN_IO, // Changed from .data_in_num
    }};
    
    // Initialize the channel in standard I2S mode.
    // This function only takes TWO arguments in your IDF version.
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));

    // 3. Enable the channel
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(TAG, "INMP441 I2S Standard initialized: BCLK=%d WS=%d DIN=%d (I2S Channel 0)", I2S_BCK_IO, I2S_WS_IO, I2S_DATA_IN_IO);
}

/* ---------- Mic test task ---------- */
void mic_test_task(void *param)
{
    mic_enabled = true;
    int32_t i2s_buf[SAMPLE_BUF_LEN];
    ESP_LOGI(TAG, "Starting mic test task...");

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_chan, i2s_buf, sizeof(i2s_buf), &bytes_read, portMAX_DELAY);
        if (ret == ESP_OK && bytes_read > 0) {
            size_t samples = bytes_read / sizeof(int32_t);

            // Convert first sample to 16-bit PCM
            int16_t s16 = (int16_t)(i2s_buf[0] >> 8);

            // Log the first sample and total samples
            ESP_LOGI(TAG, "Mic first sample (s16) = %d (samples=%zu bytes=%zu)", s16, samples, bytes_read);

            // Log first 10 samples in hex
            printf("First 10 i2s32 samples: ");

            for (int i = 0; i < 10 && i < samples; i++) {
                printf("i2s32[%d] = 0x%08" PRIx32 "\n", i, i2s_buf[i]);
            }
            printf("\n");
        } else {
            ESP_LOGW(TAG, "I2S read failed: %d", ret);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


/* ---------- I2S capture task ---------- */
void i2s_capture_task(void *param)
{
    ESP_LOGI(TAG, "i2s_capture_task start");

    int32_t i2s32[AUDIO_FRAME_SAMPLES];
    int16_t pcm16[AUDIO_FRAME_SAMPLES];

    while (1) {
        static bool mic_was_enabled = false;
        if (!mic_enabled) {
            if (mic_was_enabled) {
                ESP_LOGI(TAG, "[MIC] Capture stopped");
                mic_was_enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        } else if (!mic_was_enabled) {
            ESP_LOGI(TAG, "[MIC] Capture started");
            mic_was_enabled = true;
        }

        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(rx_chan, i2s32, sizeof(i2s32), &bytes_read, pdMS_TO_TICKS(200));
        if (r != ESP_OK || bytes_read == 0) continue;

        size_t samples = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < samples; i++) {
            pcm16[i] = (int16_t)(i2s32[i] >> 8); // convert 32-bit to 16-bit PCM
        }
        // Placeholder: encode/send data (PCM/ADPCM)
        ESP_LOGI(TAG, "Captured %u samples", (unsigned)samples);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


/* ---------- App main ---------- */

void app_main(void) {
    ESP_LOGI(TAG, "Main started; initializing...");

    nvs_flash_init();
    state_lock = xSemaphoreCreateMutex();
    audio_rb = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    i2s_init_inmp441();

#if USE_MIC_TEST_TASK
    xTaskCreate(mic_test_task, "mic_test", 4096, NULL, 5, NULL);
#else
    xTaskCreate(i2s_capture_task, "i2s_capture", 8192, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "Task created; ready.");
}