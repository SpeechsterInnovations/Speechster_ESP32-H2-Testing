/*
  main.c

  Stream INMP441 audio (mono) over USB CDC (stdout) as raw PCM s16le @ 16 kHz.

  - Startup delay and "PING" marker for host sync.
  - I2S producer reads 32-bit frames from INMP441 (left-aligned 24-bit)
    converts to int16_t and pushes into stream buffer.
  - stdout writer reads stream buffer slices and writes to stdout (unbuffered).
  - Heavy initialization runs in a separate task with a large stack to avoid
    stack/heap pressure problems seen during GDMA/I2S init on esp32h2.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"

static const char *TAG = "INMP441";

/* Audio / I2S configuration */
#define SAMPLE_RATE         16000
#define I2S_NUM             (0)   // i2s channel index in macros (not used directly in std driver)
#define I2S_READ_SAMPLES    256   // number of 32-bit frames per read (tunable)

/* Replace these with your wiring */
#define I2S_BCLK_PIN       GPIO_NUM_4
#define I2S_WS_PIN         GPIO_NUM_5
#define I2S_SD_PIN         GPIO_NUM_6

/* Stream buffer & writer sizes */
#define STREAMBUF_SIZE      (32 * 1024)   // 32 KB stream buffer
#define WRITE_SLICE_BYTES   256           // writer chunk size in bytes

/* Producer/writer handles (globals so tasks don't allocate them on stack) */
static StreamBufferHandle_t s_streambuf = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;

/* ---- I2S init function ----
   Conservative DMA settings to reduce early heap pressure.
*/
static esp_err_t i2s_init_conservative(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    /* reduce DMA descriptor count/frames to reduce heap at allocation time */
    chan_cfg.dma_desc_num  = 2;   // fewer descriptors
    chan_cfg.dma_frame_num = 32;  // fewer frames per descriptor

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        /* INMP441: 24-bit left aligned in 32-bit slot; use 32-bit frame, mono */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN,
        },
    };

    err = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---- Producer task: read from I2S, convert 32-bit -> int16, push to stream buffer ---- */
static void i2s_producer_task(void *arg)
{
    const size_t read_bytes = I2S_READ_SAMPLES * sizeof(int32_t);
    /* allocate on heap to keep stack light */
    int32_t *i2s_buf = (int32_t *)heap_caps_malloc(read_bytes, MALLOC_CAP_DEFAULT);
    if (!i2s_buf) {
        ESP_LOGE(TAG, "producer: heap_caps_malloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(s_rx_handle, i2s_buf, read_bytes, &bytes_read, pdMS_TO_TICKS(200));
        if (r == ESP_OK && bytes_read > 0) {
            size_t frames = bytes_read / sizeof(int32_t);

            for (size_t i = 0; i < frames; ++i) {
                int32_t s32 = i2s_buf[i];
                /* INMP441 provides 24-bit left-aligned in upper 24 bits of 32-bit word.
                   Convert to 16-bit by right-shifting: >> 14 (32-16-2) to keep sign */
                int16_t s16 = (int16_t)(s32 >> 14);
                size_t sent = xStreamBufferSend(s_streambuf, &s16, sizeof(s16), pdMS_TO_TICKS(10));
                if (sent < sizeof(s16)) {
                    /* Buffer full: drop oldest bytes to make room. */
                    const size_t drop_bytes = 512;
                    if (xStreamBufferSpacesAvailable(s_streambuf) == 0) {
                        uint8_t tmp[512];
                        size_t to_drop = (drop_bytes <= xStreamBufferBytesAvailable(s_streambuf)) ? drop_bytes :
                                          xStreamBufferBytesAvailable(s_streambuf);
                        if (to_drop) {
                            xStreamBufferReceive(s_streambuf, tmp, to_drop, 0); // drop oldest
                            /* try send again once (non-blocking) */
                            xStreamBufferSend(s_streambuf, &s16, sizeof(s16), 0);
                        }
                    }
                }
            }
        } else {
            /* no data -> yield briefly */
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    heap_caps_free(i2s_buf);
    vTaskDelete(NULL);
}

/* ---- Writer task: read stream buffer slices and write to stdout ---- */
static void stdout_writer_task(void *arg)
{
    /* Unbuffer stdout: write straight to CDC (USB) */
    setvbuf(stdout, NULL, _IONBF, 0);

    uint8_t outbuf[WRITE_SLICE_BYTES];

    while (1) {
        size_t got = xStreamBufferReceive(s_streambuf, outbuf, sizeof(outbuf), pdMS_TO_TICKS(200));
        if (got > 0) {
            /* Write chunk in one shot. Ignore partial writes; host side will drop on slow consumer. */
            size_t wrote = fwrite(outbuf, 1, got, stdout);
            (void)wrote;
            fflush(stdout);

            /* Short yield so USB UART driver and lower-priority tasks run */
            taskYIELD();
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    vTaskDelete(NULL);
}

/* ---- Heavy init & task creation runs in this dedicated task with big stack ---- */
static void init_task(void *arg)
{
    ESP_LOGI(TAG, "init_task: small startup delay to let host settle...");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Create stream buffer */
    s_streambuf = xStreamBufferCreate(STREAMBUF_SIZE, 1); // 1 byte trigger granularity
    if (!s_streambuf) {
        ESP_LOGE(TAG, "init_task: xStreamBufferCreate failed");
        vTaskDelete(NULL);
        return;
    }

    /* Initialize I2S (may allocate GDMA resources) */
    esp_err_t r = i2s_init_conservative();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "init_task: i2s_init_conservative failed");
        vTaskDelete(NULL);
        return;
    }

    /* Send a short marker to host so it can sync and remove pre-audio logs */
    puts("PING");

    /* Create producer (higher priority) and writer (lower priority) */
    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(i2s_producer_task, "i2s_prod", 4096, NULL, tskIDLE_PRIORITY + 3, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "init_task: failed to create i2s_producer_task");
        vTaskDelete(NULL);
        return;
    }

    ok = xTaskCreatePinnedToCore(stdout_writer_task, "stdout_w", 4096, NULL, tskIDLE_PRIORITY + 1, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "init_task: failed to create stdout_writer_task");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "init_task: Producer + writer started. Returning init_task (will delete itself).");

    /* delete self to free stack (we started with large stack) */
    vTaskDelete(NULL);
}

/* ---- app_main: spawn init_task on its own large stack to avoid stack/heap pressure ---- */

#include "esp_vfs_dev_uart.h"

void app_main(void)
{
    // Route stdio (printf, fwrite, etc.) to USB Serial/JTAG
    //esp_vfs_dev_usb_serial_jtag_use_driver();

    // Optional: unbuffer to avoid stalls
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "stdout routed to USB Serial/JTAG");

    /* now safe to use puts("PING") and fwrite() */
    BaseType_t ok = xTaskCreatePinnedToCore(init_task, "init_task", 8192, NULL, 2, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create init_task");
    }
}

