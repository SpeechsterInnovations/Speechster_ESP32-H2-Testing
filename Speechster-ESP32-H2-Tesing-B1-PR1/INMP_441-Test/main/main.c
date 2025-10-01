/*
 * main.c
 *
 * Hybrid logger + UART command interface (systemctl-like) for ESP32-H2
 * Integrated real I2S std-mode (INMP441), fixed-time 5s capture, WAV header
 *
 * Pins:
 *   SD -> GPIO10
 *   SCK -> GPIO4
 *   WS -> GPIO5
 *
 * Build:
 *   idf.py build
 *   idf.py -p /dev/ttyACM0 flash monitor
 *
 * Notes:
 * - Add Kconfig.projbuild entry (optional) to control CONFIG_ENABLE_MAX_DEBUG
 * - For debug flooding set ENABLE_MAX_DEBUG compile define (or menuconfig)
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "sdkconfig.h"

// -----------------------------------------------------------------------------
// Configuration / toggles
// -----------------------------------------------------------------------------

// manual compile-time override (uncomment to force)
 // #define ENABLE_MAX_DEBUG 1

#if CONFIG_ENABLE_MAX_DEBUG
    #ifndef ENABLE_MAX_DEBUG
        #define ENABLE_MAX_DEBUG 1
    #endif
#endif

// Runtime macro-like constants
#define UART_PORT_NUM      UART_NUM_0    // USB-CDC (ttyACM0)
#define UART_BAUD_RATE     115200        // set higher if you can (921600, 1152000)
#define UART_RX_BUF_SIZE   1024
#define UART_TX_BUF_SIZE   4096

#define FRAME_START_MARKER "<FRAME_START>\n"
#define FRAME_END_MARKER   "<FRAME_END>\n"

#define SAMPLE_RATE        16000
#define BITS_PER_SAMPLE    16
#define CHANNELS           1
#define RECORD_SECONDS     5

// I2S pins (INMP441)
#define I2S_SD_PIN  10
#define I2S_SCK_PIN 4
#define I2S_WS_PIN  5

// stream chunk size for sending WAV body over UART
#define STREAM_CHUNK_SZ    1024

// frame buffer for raw I2S read (bytes)
#define FRAME_SIZE         512

// -----------------------------------------------------------------------------
// Tags
// -----------------------------------------------------------------------------
static const char *TAG_APP    = "APP";
static const char *TAG_I2S    = "I2S";
static const char *TAG_UART   = "UART";
static const char *TAG_STREAM = "STREAM";
static const char *TAG_CMD    = "CMD";

// -----------------------------------------------------------------------------
// Service model + counters
// -----------------------------------------------------------------------------
typedef enum {
    SERVICE_IDLE = 0,
    SERVICE_RUNNING,
    SERVICE_ERROR,
} service_state_t;

static volatile bool verbose_enabled = true;  // hybrid: periodic logs enabled by default

static volatile service_state_t svc_main_state   = SERVICE_RUNNING;
static volatile service_state_t svc_i2s_state    = SERVICE_IDLE;
static volatile service_state_t svc_uart_state   = SERVICE_RUNNING;
static volatile service_state_t svc_stream_state = SERVICE_IDLE;
static volatile service_state_t svc_logger_state = SERVICE_RUNNING;

static volatile uint32_t i2s_frames_captured = 0;
static volatile uint32_t stream_frames_sent  = 0;
static volatile uint32_t uart_write_errors   = 0;

// -----------------------------------------------------------------------------
// I2S channel handle
// -----------------------------------------------------------------------------
static i2s_chan_handle_t rx_chan = NULL;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static const char *service_state_name(service_state_t s) {
    switch (s) {
    case SERVICE_IDLE:     return "IDLE";
    case SERVICE_RUNNING:  return "RUNNING";
    case SERVICE_ERROR:    return "ERROR";
    default:               return "UNKNOWN";
    }
}

static void set_service_state(volatile service_state_t *svc, service_state_t new_state) {
    *svc = new_state;
}

// -----------------------------------------------------------------------------
// safe UART write (handles partial writes, increments error counters)
// -----------------------------------------------------------------------------
static esp_err_t safe_uart_write(const uint8_t *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        int w = uart_write_bytes(UART_PORT_NUM, (const char*)(data + written), len - written);
        if (w < 0) {
            uart_write_errors++;
            ESP_LOGE(TAG_UART, "uart_write_bytes error: %d", w);
            return ESP_FAIL;
        }
        written += (size_t)w;
        // small yield to allow driver flush and other tasks to run
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// status_task: periodic macOS-like logs
// -----------------------------------------------------------------------------
static void status_task(void *arg)
{
    const TickType_t delay = pdMS_TO_TICKS(1000);

    while (1) {
        if (verbose_enabled) {
            uint64_t us = esp_timer_get_time();
            uint64_t ms = us / 1000ULL;
            ESP_LOGI(TAG_APP, "[%llu ms] MAIN  state=%s", (unsigned long long)ms, service_state_name(svc_main_state));
            ESP_LOGI(TAG_APP, "[%llu ms] I2S   state=%s frames=%" PRIu32, (unsigned long long)ms, service_state_name(svc_i2s_state), i2s_frames_captured);
            ESP_LOGI(TAG_APP, "[%llu ms] UART  state=%s write_errors=%" PRIu32, (unsigned long long)ms, service_state_name(svc_uart_state), uart_write_errors);
            ESP_LOGI(TAG_APP, "[%llu ms] STREAM state=%s frames_sent=%" PRIu32, (unsigned long long)ms, service_state_name(svc_stream_state), stream_frames_sent);
        }
        vTaskDelay(delay);
    }
}

// -----------------------------------------------------------------------------
// command_task: UART command parser (list-services, status <svc>, quiet, verbose)
// -----------------------------------------------------------------------------
static void send_response(const char *s)
{
    safe_uart_write((const uint8_t*)s, strlen(s));
    safe_uart_write((const uint8_t*)"\r\n", 2);
}

static void command_task(void *arg)
{
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            buf[len] = '\0';
            char *p = (char*)buf;
            while (*p == ' ' || *p == '\t') p++;

            if (strstr(p, "list-services") != NULL) {
                send_response("Services: MAIN, I2S, UART, STREAM, LOGGER");
            } else if (strncmp(p, "status", 6) == 0) {
                char service[32] = {0};
                int n = sscanf(p + 6, " %31s", service);
                if (n == 1) {
                    if (strcasecmp(service, "I2S") == 0) {
                        char out[256];
                        snprintf(out, sizeof(out), "[I2S] state=%s frames=%" PRIu32, service_state_name(svc_i2s_state), i2s_frames_captured);
                        send_response(out);
                    } else if (strcasecmp(service, "UART") == 0) {
                        char out[128];
                        snprintf(out, sizeof(out), "[UART] state=%s write_errors=%" PRIu32, service_state_name(svc_uart_state), uart_write_errors);
                        send_response(out);
                    } else if (strcasecmp(service, "STREAM") == 0) {
                        char out[128];
                        snprintf(out, sizeof(out), "[STREAM] state=%s frames_sent=%" PRIu32, service_state_name(svc_stream_state), stream_frames_sent);
                        send_response(out);
                    } else if (strcasecmp(service, "MAIN") == 0) {
                        char out[128];
                        snprintf(out, sizeof(out), "[MAIN] state=%s", service_state_name(svc_main_state));
                        send_response(out);
                    } else {
                        send_response("Unknown service name");
                    }
                } else {
                    send_response("usage: status <SERVICE>");
                }
            } else if (strstr(p, "quiet") != NULL) {
                verbose_enabled = false;
                send_response("Verbose logs DISABLED");
            } else if (strstr(p, "verbose") != NULL) {
                verbose_enabled = true;
                send_response("Verbose logs ENABLED");
            } else {
                char tmp[320];
                // trim linebreaks for echo
                char *nl = strchr(p, '\r'); if (nl) *nl = '\0';
                nl = strchr(p, '\n'); if (nl) *nl = '\0';
                snprintf(tmp, sizeof(tmp), "Unknown command: %s", p);
                send_response(tmp);
            }
            memset(buf, 0, sizeof(buf));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// -----------------------------------------------------------------------------
// I2S init (std-mode) for INMP441
// -----------------------------------------------------------------------------
static esp_err_t init_i2s_std_for_inmp441(void)
{
    ESP_LOGI(TAG_I2S, "Creating I2S channel (std-mode, master RX)...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // create rx channel: second param is tx handle output pointer (we don't use TX)
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_I2S, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // std driver config: clock, slot, gpio
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN
        }
    };

    ret = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_I2S, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG_I2S, "I2S initialized (sr=%d, bits=16, mono)", SAMPLE_RATE);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// capture_task: uses real I2S reads, sends PCM over UART as WAV (markers used)
// -----------------------------------------------------------------------------
// capture_task() - fixed-time recording (5s) -> sends FRAME_START and FRAME_END markers
static void capture_task(void *arg)
{
    ESP_LOGI(TAG_APP, "Capture task starting for %d seconds...", RECORD_SECONDS);

    const size_t max_bytes = RECORD_SECONDS * SAMPLE_RATE * (BITS_PER_SAMPLE / 8);
    const size_t progress_interval = (max_bytes / 10) ? (max_bytes / 10) : FRAME_SIZE;

    uint8_t *buf = malloc(FRAME_SIZE);
    if (!buf) {
        ESP_LOGE(TAG_APP, "Failed to allocate capture buffer (%u bytes)", (unsigned)FRAME_SIZE);
        set_service_state(&svc_i2s_state, SERVICE_ERROR);
        vTaskDelete(NULL);
        return;
    }

    // 🔹 Send FRAME_START marker only
    if (safe_uart_write((const uint8_t*)FRAME_START_MARKER, strlen(FRAME_START_MARKER)) != ESP_OK) {
        ESP_LOGW(TAG_UART, "Failed to send FRAME_START marker (continuing)");
    }

    // Enable I2S
    esp_err_t r = i2s_channel_enable(rx_chan);
    if (r != ESP_OK) {
        ESP_LOGE(TAG_I2S, "Failed to enable I2S channel: %s", esp_err_to_name(r));
        set_service_state(&svc_i2s_state, SERVICE_ERROR);
        free(buf);
        vTaskDelete(NULL);
        return;
    }
    set_service_state(&svc_i2s_state, SERVICE_RUNNING);

    size_t total_bytes = 0;
    size_t bytes_read = 0;
    size_t dropped_frames = 0;
    size_t uart_retries = 0;

    while (total_bytes < max_bytes) {
        r = i2s_channel_read(rx_chan, buf, FRAME_SIZE, &bytes_read, pdMS_TO_TICKS(200));
        if (r != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TAG_I2S, "I2S read timeout/error: %s (bytes_read=%u)", esp_err_to_name(r), (unsigned)bytes_read);
            dropped_frames++;
            continue;
        }

        // 🔹 Send PCM directly over UART
        size_t offset = 0;
        while (offset < bytes_read) {
            int sent = uart_write_bytes(UART_PORT_NUM, (const char*)(buf + offset), bytes_read - offset);
            if (sent < 0) {
                ESP_LOGE(TAG_UART, "UART write failed at offset %u", (unsigned)offset);
                uart_write_errors++;
                uart_retries++;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            offset += (size_t)sent;
        }

        total_bytes += bytes_read;
        i2s_frames_captured++;

#ifdef ENABLE_MAX_DEBUG
        ESP_LOGI(TAG_STREAM, "Chunk %u bytes (total %u / %u)", (unsigned)bytes_read, (unsigned)total_bytes, (unsigned)max_bytes);
#else
        if ((total_bytes % progress_interval) < FRAME_SIZE) {
            ESP_LOGI(TAG_STREAM, "Progress: %u/%u bytes (%.1f%%)", (unsigned)total_bytes, (unsigned)max_bytes, 100.0 * total_bytes / max_bytes);
        }
#endif
    }

    // Disable I2S
    r = i2s_channel_disable(rx_chan);
    if (r != ESP_OK) {
        ESP_LOGW(TAG_I2S, "i2s_channel_disable returned %s", esp_err_to_name(r));
    }
    set_service_state(&svc_i2s_state, SERVICE_IDLE);

    // 🔹 Send FRAME_END marker
    if (safe_uart_write((const uint8_t*)FRAME_END_MARKER, strlen(FRAME_END_MARKER)) != ESP_OK) {
        ESP_LOGW(TAG_UART, "Failed to send FRAME_END marker");
    }

    ESP_LOGI(TAG_APP, "Capture complete: total_bytes=%u (expected %u), dropped=%u, uart_retries=%u",
             (unsigned)total_bytes, (unsigned)max_bytes, (unsigned)dropped_frames, (unsigned)uart_retries);

    free(buf);
    vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelete(NULL);
}



// -----------------------------------------------------------------------------
// UART init
// -----------------------------------------------------------------------------
static void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    // install driver
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));

    // For USB-CDC (USB native), pins are not used; set to no-change
    uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    set_service_state(&svc_uart_state, SERVICE_RUNNING);
    ESP_LOGI(TAG_UART, "UART initialized at %d baud (port %d)", UART_BAUD_RATE, UART_PORT_NUM);
}

// -----------------------------------------------------------------------------
// enable_max_debug() helper - call from app_main if you want to always enable
// -----------------------------------------------------------------------------
static void enable_max_debug(void)
{
#ifdef ENABLE_MAX_DEBUG
    ESP_LOGW(TAG_APP, "MAX DEBUG: Enabled compile-time (ENABLE_MAX_DEBUG)");
#else
    // if CONFIG_ENABLE_MAX_DEBUG is set, it maps to ENABLE_MAX_DEBUG in top of file
 #if CONFIG_ENABLE_MAX_DEBUG
    ESP_LOGW(TAG_APP, "MAX DEBUG: Enabled via menuconfig (CONFIG_ENABLE_MAX_DEBUG)");
 #else
    ESP_LOGI(TAG_APP, "MAX DEBUG: not enabled");
 #endif
#endif
}

// -----------------------------------------------------------------------------
// app_main
// -----------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG_APP, "App init: hybrid logger + I2S capture (INMP441) demo");

    // optionally enable max debug by calling the helper:
    // enable_max_debug();

    // init UART
    init_uart();

    // init I2S
    if (init_i2s_std_for_inmp441() != ESP_OK) {
        ESP_LOGE(TAG_I2S, "I2S init failed - capture won't run");
        set_service_state(&svc_i2s_state, SERVICE_ERROR);
    }

    // spawn status and command tasks
    xTaskCreate(status_task, "status_task", 4096, NULL, 1, NULL);
    xTaskCreate(command_task, "command_task", 4096, NULL, 2, NULL);

    // spawn capture task (fixed 5s)
    xTaskCreate(capture_task, "capture_task", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG_APP, "Initialization complete.");
    // keep main alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
