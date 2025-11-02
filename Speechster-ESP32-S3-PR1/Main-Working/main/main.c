// ─────────────────────────────────────────────
// Speechster-B1 BLE Audio Streamer — v1 STABLE --- Added WiFi Audio Streaming
// Nov-2025 build (ESP-IDF v6.1.0)
// Safe aggregation, adaptive BLE pacing, heap-based buffers
// Built for ESP32-S3-DevKitC-1-N8R2 ESP32-S3 WiFi Bluetooth-Compatible BLE 5.0 Mesh Development Board
// ─────────────────────────────────────────────

//#pragma GCC diagnostic ignored "-Wcpp"

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
#include "freertos/portmacro.h"
#include "esp_heap_trace.h"
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/portable.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include <stdatomic.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_cache.h"

#include "driver/i2s_std.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "hal/adc_types.h" 

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
#include "esp_bt.h"

/* JSON parsing */
#include "cJSON.h"

/* WiFi Includes */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "freertos/semphr.h"
#include "esp_websocket_client.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"

#include "driver/temperature_sensor.h"   // on-chip temp sensor (ESP32-S3)

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "speechster_b1";

/* ---------- Defaults & config ---------- */
#define UART_BAUD_RATE       115200
#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_FRAME_MS       10
#define AUDIO_FRAME_SAMPLES   ((AUDIO_SAMPLE_RATE / 1000) * AUDIO_FRAME_MS)  // 960 at 48kHz
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_READ_BYTES     (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_RINGBUF_SIZE   (48 * 1024)
#define STREAM_SLICE_MAX   (8 * 1024) // Smaller slice size reduces total number of chunks per logical frame
#define I2S_BUF_SIZE 1024
#define AGGREGATE_BYTES  (15 * 320)
#define LOG_CTRL(fmt, ...) ESP_LOGI(TAG, "[CTRL_IN] " fmt, ##__VA_ARGS__)
#define LOG_JSON_PARSE(fmt, ...) ESP_LOGI(TAG, "[JSON_PARSE] " fmt, ##__VA_ARGS__)
// #define LOG_STREAM(fmt, ...) ESP_LOGI(TAG, "[STREAM_OUT] " fmt, ##__VA_ARGS__)
#define HEAP_TRACE_RECORDS 256

/* ---------- Heap-safe pre-allocated buffers ---------- */
#define MAX_FRAME_SIZE   (8 + AUDIO_READ_BYTES)  // 8-byte header + PCM
static uint8_t audio_frame_buf[MAX_FRAME_SIZE]; // pre-allocated buffer for PCM frames

static i2s_chan_handle_t rx_chan = NULL; 
static uint32_t rb_overflow_count = 0;
static uint32_t ble_delay_ms = 10;

#define I2S_PORT_NUM         I2S_NUM_0
#define I2S_BCK_IO           14
#define I2S_WS_IO            15
#define I2S_DATA_IN_IO       16
#define I2S_NUM         I2S_NUM_0
#define SAMPLE_BUF_LEN  320  // Number of samples per read

// Top-level globals
static int32_t *i2s_dma_buf = NULL;
static int16_t *pcm_dma_buf = NULL;
static adc_continuous_handle_t adc_handle = NULL;
static QueueHandle_t adc_evt_queue = NULL;
static bool adc_running = false;

static temperature_sensor_handle_t temp_sensor_handle = NULL;
static float current_temp_c = 0.0f;

/* PM config (tunable) */
#ifndef SPEECHSTER_PM_MAX_MHZ
#define SPEECHSTER_PM_MAX_MHZ 160
#endif
#ifndef SPEECHSTER_PM_MIN_MHZ
#define SPEECHSTER_PM_MIN_MHZ 80
#endif

/* Thermal thresholds (adjust to your enclosure / testing) */
#define THERMAL_WARN_C   65.0f   // throttling threshold
#define THERMAL_CRIT_C   75.0f   // emergency stop threshold

// FSR Stuff
#define ADC_UNIT            ADC_UNIT_1
#define ADC_ATTEN           ADC_ATTEN_DB_12
#define ADC_BIT_WIDTH       SOC_ADC_DIGI_MAX_BITWIDTH
#define ADC_READ_LEN        256   // bytes per read (tune as needed)
#define ADC_CONV_MODE       ADC_CONV_SINGLE_UNIT_1
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define FSR_ADC_CHANNEL ADC_CHANNEL_6
#define CONSERVATIVE_LL_OCTETS 220
static uint32_t fsr_interval_ms = 50; // default 50ms, can be changed via JSON

/* Packet framing (8 byte header):
 * 0: type (0x01 audio, 0x02 sensor)
 * 1: flags (reserved, currently always 0x00)
 * 2-3: seq (uint16 little-endian)
 * 4-7: ts_ms (uint32 little-endian)
 * 5: Payload
 */
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

/* ---------- 16-bit additive checksum ---------- */
static __attribute__((unused)) uint16_t calc_checksum16(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    // Fold to 16 bits and wrap carry
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}


/* Runtime State */
static const uint8_t AUDIO_FLAG_PCM = 0x00;
static volatile bool pSensor_enabled = false;
static uint32_t seq_counter = 0;
static uint8_t *agg_buf = NULL;
static volatile uint64_t idle_ticks = 0;
static uint64_t last_total_ticks = 0;
static SemaphoreHandle_t ringbuf_mutex = NULL; // Protects ringbuffer access without disabling interrupts

/* ------------------- WiFi Config ------------------- */
static const char *WIFI_TAG = "WiFiSend";
static bool wifi_connected = false;

// Dynamic server URIs — filled at runtime from BLE credentials or NVS
static char WS_CTRL_URI[128]  = {0};   // ws://<host>:<port>/ws    (control + telemetry)
static char WS_AUDIO_URI[128] = {0};   // ws://<host>:<port>/data/audio  (binary audio)
static char OTA_URL[128]      = {0};
static char TELEMETRY_URL[128]= {0};

// Two websocket clients: control and audio
static esp_websocket_client_handle_t ws_client_ctrl  = NULL;
static esp_websocket_client_handle_t ws_client_audio = NULL;

// Per-client connected flags
static bool ws_ctrl_connected  = false;
static bool ws_audio_connected = false;

static esp_http_client_handle_t wifi_http_client = NULL;

// ─────────────────────────────────────────────
// Wi-Fi frame send queue (decoupled from I2S task)
// ─────────────────────────────────────────────
#define WIFI_SEND_QUEUE_LEN 8
typedef struct {
    size_t len;
    uint8_t data[MAX_FRAME_SIZE];
} wifi_frame_t;

static QueueHandle_t wifi_send_q = NULL;


#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

static SemaphoreHandle_t state_lock = NULL;

/* BLE state */
static uint16_t g_conn_handle = 0;
static uint16_t g_stream_attr_handle = 0; // populated on subscribe
static volatile bool fake_mode = false; // start with fake frames disabled

/* Control command queue for deferred JSON parsing */
#define CTRL_QUEUE_LEN 8
typedef struct {
    size_t len;
    char *json;
} control_msg_t;
static QueueHandle_t ctrl_queue = NULL;

// Forward declarations
static int send_chunked_notify(uint16_t conn_handle,
                               uint16_t attr_handle,
                               const uint8_t *frame,
                               size_t frame_len);

static bool start_mic_streaming(void);
static void stop_mic_streaming(void);
void perform_ota_update(void *pvParameters);
static void ble_send_json_response(const char *json_str);

// ======== Concurrency Flags ========
static atomic_bool mic_active = ATOMIC_VAR_INIT(false);
static atomic_bool bt_conn = ATOMIC_VAR_INIT(false);
static atomic_bool streaming = ATOMIC_VAR_INIT(false);

// ======== BLE Sender Backoff State ========
static uint32_t ble_backoff_ms = 5;

// ======== Health Monitoring ========
static uint32_t frames_sent = 0;

static adc_cali_handle_t adc_cali_handle = NULL;

static __attribute__((unused)) void adc_cali_init(void) {
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_handle) == ESP_OK) {
        ESP_LOGI("ADC_CALI", "Calibration enabled");
    } else {
        ESP_LOGW("ADC_CALI", "Calibration not supported on this chip");
    }
}

/* ---- ADC ISR callback ---- */
static bool adc_conv_done_cb(adc_continuous_handle_t handle,
                             const adc_continuous_evt_data_t *edata,
                             void *user_data)
{
    BaseType_t mustYield = pdFALSE;

    if (adc_evt_queue) {
        /* try to push the event size into the queue; count drops */
        if (xQueueSendFromISR(adc_evt_queue, &edata->size, &mustYield) != pdTRUE) {
            /* Track dropped ADC events for telemetry */
            rb_overflow_count++;
        }
    }

    /* Return true if a context switch is required */
    return mustYield;
}


static void adc_start(void)
{
    if (adc_running) {
        ESP_LOGW("ADC_CONT", "ADC already running");
        return;
    }

    if (!adc_handle) {
        adc_evt_queue = xQueueCreate(8, sizeof(uint32_t));

        adc_continuous_handle_cfg_t handle_cfg = {
            .max_store_buf_size = 1024,
            .conv_frame_size = ADC_READ_LEN,
        };
        ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

        // ADC pattern setup
        static adc_digi_pattern_config_t adc_pattern[1];
        adc_pattern[0].atten      = ADC_ATTEN_DB_12;
        adc_pattern[0].channel    = FSR_ADC_CHANNEL;
        adc_pattern[0].unit       = ADC_UNIT_1;
        adc_pattern[0].bit_width  = SOC_ADC_DIGI_MAX_BITWIDTH;

        adc_continuous_config_t dig_cfg = {
            .sample_freq_hz = 1000,
            .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
            .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
            .adc_pattern    = adc_pattern,
            .pattern_num    = 1,
        };

        ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));

        adc_continuous_evt_cbs_t cbs = {
            .on_conv_done = adc_conv_done_cb,
        };
        ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_handle, &cbs, NULL));
    }

    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
    adc_running = true;
    ESP_LOGI("ADC_CONT", "ADC started (continuous mode)");
}

static void adc_stop(void)
{
    if (!adc_running || !adc_handle) return;
    ESP_ERROR_CHECK(adc_continuous_stop(adc_handle));
    adc_running = false;
    ESP_LOGI("ADC_CONT", "ADC stopped");
}

/* ---------- I2S (INMP441) ---------- */
static void i2s_init_inmp441(void) {
    i2s_chan_config_t chan_cfg = {
        .id = I2S_PORT_NUM,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4,
        .dma_frame_num = 128,
        .auto_clear = true,
    };

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
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

// ─────────────────────────────────────────────
// Unified JSON control handler
// ─────────────────────────────────────────────
static void handle_control_json(cJSON *root) {
    if (!root) return;

    // BLE priority: if BLE is connected, ignore WebSocket unless BLE is idle
    if (atomic_load(&bt_conn)) {
        ESP_LOGI(TAG, "BLE connected — BLE commands take priority.");
    } else if (ws_ctrl_connected) {
        ESP_LOGI(TAG, "No BLE connection — accepting WebSocket command.");
    } else {
        ESP_LOGW(TAG, "No control channel active; ignoring JSON command.");
        return;
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);

    cJSON *jmic = cJSON_GetObjectItem(root, "mic");
    if (jmic && cJSON_IsBool(jmic)) {
        if (cJSON_IsTrue(jmic)) {
            vTaskDelay(pdMS_TO_TICKS(300));
            start_mic_streaming();
            vTaskDelay(pdMS_TO_TICKS(10)); // let scheduler breathe
        } else {
            stop_mic_streaming();
        }
        LOG_JSON_PARSE("mic=%d", (int)atomic_load(&mic_active));
    }

    cJSON *jpSensor = cJSON_GetObjectItem(root, "pSensor");
    if (jpSensor && cJSON_IsBool(jpSensor)) {
        bool new_state = cJSON_IsTrue(jpSensor);
        if (new_state != pSensor_enabled) {
            pSensor_enabled = new_state;
            LOG_JSON_PARSE("pSensor=%d", pSensor_enabled);

            if (pSensor_enabled) {
                adc_start();
            } else {
                adc_stop();
            }
        }
    }

    cJSON *jdelay = cJSON_GetObjectItem(root, "ble_delay_ms");
    if (jdelay && cJSON_IsNumber(jdelay)) {
        uint32_t v = (uint32_t) jdelay->valuedouble;
        if (v >= 5 && v <= 200) {
            ble_delay_ms = v;
            LOG_JSON_PARSE("ble_delay_ms=%u", ble_delay_ms);
        }
    }

    cJSON *jcmd = cJSON_GetObjectItem(root, "cmd");
    if (jcmd && cJSON_IsString(jcmd)) {
        const char *cmd = jcmd->valuestring;
        if (strcmp(cmd, "OTA_UPDATE") == 0) {
            xTaskCreate(&perform_ota_update, "ota_task", 8192, NULL, 5, NULL);
        }
    }

    xSemaphoreGive(state_lock);
}


void mic_test_task_debug(void *param) {
    atomic_store(&mic_active, true);

    int32_t buf[256];
    size_t bytes_read = 0;
    ESP_LOGI(TAG, "Starting mic test task...");

    while (1) {
        esp_err_t ret = i2s_channel_read(rx_chan, buf, sizeof(buf), &bytes_read, portMAX_DELAY);
        if (ret == ESP_OK && bytes_read > 0) {
            size_t samples = bytes_read / sizeof(int32_t);
            int32_t avg = 0;
            for (size_t i = 0; i < samples; i++) avg += (buf[i] >> 14);
            avg /= samples;
            ESP_LOGI(TAG, "Mic avg raw: %ld", avg);
        } else {
            ESP_LOGW(TAG, "I2S read failed: %d", ret);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool start_mic_streaming(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&mic_active, &expected, true)) {
        ESP_LOGW(TAG, "Mic already active");
        return false;
    }
    atomic_store(&streaming, true);
    return true;
}

static void stop_mic_streaming(void) {
    atomic_store(&streaming, false);
    atomic_store(&mic_active, false);
}

// ─────────────────────────────────────────────
// Dedicated Wi-Fi sender task — async HTTP/WS sends
// ─────────────────────────────────────────────
static void wifi_send_task(void *arg)
{
    wifi_frame_t frame;

    ESP_LOGI("WiFiSendTask", "Wi-Fi sender task started");

    while (1) {
        if (xQueueReceive(wifi_send_q, &frame, portMAX_DELAY) == pdTRUE) {
            if (ws_client_audio && esp_websocket_client_is_connected(ws_client_audio)) {
                esp_websocket_client_send_bin(ws_client_audio, (const char *)frame.data, frame.len, portMAX_DELAY);
            } else {
                ESP_LOGW("WiFiSendTask", "Audio WS not ready, dropping frame");
            }
        }
    }
}

/* ---------- I2S capture task (DMA-safe, cache-coherent) ---------- */
static void i2s_capture_task(void *arg)
{
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "i2s_capture_task start (Wi-Fi-only streaming mode)");

    const uint32_t frame_ms = 10; // 10 ms per read
    const size_t frame_samples = (AUDIO_SAMPLE_RATE / 1000) * frame_ms; // 160 samples @16kHz
    const size_t bytes_per_read = frame_samples * sizeof(int32_t);       // bytes read from i2s
    const size_t pcm_bytes = frame_samples * sizeof(int16_t);           // downmixed bytes
    const TickType_t i2s_read_timeout = pdMS_TO_TICKS(8);

    // Sanity: ensure global DMA buffers are allocated and big enough
    if (!i2s_dma_buf || !pcm_dma_buf) {
        ESP_LOGE(TAG, "i2s_capture_task: DMA buffers not allocated");
        vTaskDelete(NULL);
        return;
    }

    // If you allocated larger buffers earlier (e.g. 2048), it's safe to use bytes_per_read here
    // Zero the regions we'll use (optional)
    memset(i2s_dma_buf, 0, bytes_per_read);
    memset(pcm_dma_buf, 0, pcm_bytes);

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(0);

        if (!atomic_load(&mic_active) || !ws_audio_connected) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_chan, (void *)i2s_dma_buf, bytes_per_read, &bytes_read, i2s_read_timeout);

        // Ensure CPU cache coherency before reading DMA-filled buffer.
        // Use the direction flag that corresponds to peripheral -> memory (M2C).
        // If your IDF variant doesn't expose ESP_CACHE_MSYNC_FLAG_DIR_M2C, check your headers (some versions use C2M).
        #ifdef ESP_CACHE_MSYNC_FLAG_DIR_M2C
                esp_cache_msync(i2s_dma_buf, bytes_read, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        #elif defined(ESP_CACHE_MSYNC_FLAG_DIR_C2M)
                // If only C2M is present in your IDF, use it (observed on some IDF releases).
                esp_cache_msync(i2s_dma_buf, bytes_read, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        #else
                // Fallback: no-op - but prefer to update IDF or call the correct cache API on your platform.
        #endif

        esp_task_wdt_reset();

        if (ret != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        size_t samples = bytes_read / sizeof(int32_t);
        if (samples > frame_samples) samples = frame_samples;

        // Downsample (24bit >> 16bit)

        int32_t *src = i2s_dma_buf;
        int16_t *dst = pcm_dma_buf;
        for (size_t i = 0; i < samples; i++) {
            *dst++ = (int16_t)(*src++ >> 8);
        }

        size_t frame_len = 8 + (samples * sizeof(int16_t));
        if (frame_len > MAX_FRAME_SIZE) frame_len = MAX_FRAME_SIZE;

        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint16_t seq = (uint16_t)__atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
        pack_header(audio_frame_buf, 0x01, AUDIO_FLAG_PCM, seq, ts);
        memcpy(audio_frame_buf + 8, (uint8_t *)pcm_dma_buf, frame_len - 8);

        wifi_frame_t frame;
        frame.len = frame_len;
        memcpy(frame.data, audio_frame_buf, frame_len);
        if (xQueueSend(wifi_send_q, &frame, 0) != pdTRUE) {
            ESP_LOGW("WiFiQueue", "Wi-Fi send queue full, dropping frame");
        }

        esp_task_wdt_reset();
        taskYIELD(); // micro breathing delay
    }

    // Do NOT free global buffers here (they are owned by whole-app lifecycle)
    vTaskDelete(NULL);
}

/* ---------- FSR ADC (BLE Notify) ---------- */
static void pSensor_task(void *arg)
{
    static adc_digi_output_data_t result[ADC_READ_LEN / sizeof(adc_digi_output_data_t)];
    static uint8_t fsr_frame[8 + 2]; // header + 2B payload
    adc_digi_output_data_t *entry;
    uint8_t payload[2];
    uint32_t evt_size = 0;

    ESP_LOGI("ADC_CONT", "FSR pSensor_task running (event-driven)");

    while (1) {
        if (!adc_running || !pSensor_enabled) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (xQueueReceive(adc_evt_queue, &evt_size, pdMS_TO_TICKS(fsr_interval_ms))) {
            uint32_t bytes_read = 0;
            esp_err_t ret = adc_continuous_read(adc_handle, (uint8_t *)result, sizeof(result), &bytes_read, 0);
            if (ret != ESP_OK || bytes_read < sizeof(adc_digi_output_data_t)) continue;

            size_t entries = bytes_read / sizeof(adc_digi_output_data_t);
            entry = (adc_digi_output_data_t *)result;

            for (size_t i = 0; i < entries; i++) {
                if (entry[i].type2.channel != FSR_ADC_CHANNEL) continue;

                int raw = entry[i].type2.data;
                
                static int last_raw = 0;
                raw = (last_raw * 7 + raw) / 8;  // 0.875 smoothing
                last_raw = raw;

                int mv = 0;
                if (adc_cali_handle) adc_cali_raw_to_voltage(adc_cali_handle, raw, &mv);
                payload[0] = mv & 0xFF;
                payload[1] = (mv >> 8) & 0xFF;

                uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
                uint16_t seq = (uint16_t)__atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);

                pack_header(fsr_frame, 0x02, 0x00, seq, ts);
                memcpy(fsr_frame + 8, payload, sizeof(payload));

                size_t frame_len = 8 + sizeof(payload);

                if (atomic_load(&bt_conn) && g_stream_attr_handle != 0) {
                    int rc = send_chunked_notify(g_conn_handle, g_stream_attr_handle, fsr_frame, frame_len);
                    if (rc != 0)
                        ESP_LOGW("ADC_CONT", "BLE notify failed (raw=%d) rc=%d", raw, rc);
                }
            }
        }
    }
}

/* ---- Deferred JSON parsing task ---- */
static void control_task(void *arg)
{
    control_msg_t msg;
    while (1) {
        if (xQueueReceive(ctrl_queue, &msg, portMAX_DELAY) == pdTRUE) {
            LOG_CTRL("rx JSON: %s", msg.json);
            char ack_buf[64];
            snprintf(ack_buf, sizeof(ack_buf), "{\"status\":\"RCVD\",\"msg\":\"%.32s\"}", msg.json);
            ble_send_json_response(ack_buf);


            cJSON *root = cJSON_Parse(msg.json);
            if (root) {
                handle_control_json(root);
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "[CTRL_ERR] Invalid JSON");
            }

            free(msg.json);
        }
    }
}

/* ---------- GATT: control (write JSON) ---------- */
static int gatt_control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0) return 0;

    char *json = malloc(len + 1);
    if (!json) return 0;

    os_mbuf_copydata(ctxt->om, 0, len, json);
    json[len] = '\0';

    /* Instead of parsing JSON here, just enqueue it */
    control_msg_t msg = { .len = len, .json = json };
    if (xQueueSend(ctrl_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "Control queue full; dropping JSON");
        free(json);
    }

    return 0;
}

/* ---------- Pre-allocated notification buffer ---------- */
#define MAX_NOTIFY_BUF  260
static uint8_t notify_buf[MAX_NOTIFY_BUF];

/* ---------- Heap-free BLE sender task (patched footer handling) ---------- */
// returns 0 on success (frame sent), -1 on failure (dropped)
static int send_chunked_notify(uint16_t conn_handle,
                               uint16_t attr_handle,
                               const uint8_t *frame,
                               size_t frame_len)
{
    if (!atomic_load(&bt_conn) || attr_handle == 0)
        return -1;

    esp_task_wdt_reset();

    uint16_t mtu = ble_att_mtu(conn_handle);
    size_t chunk_size = (mtu > 23) ? (mtu - 3) : 20;
    if (mtu > 100)
        chunk_size = mtu - 7;  // use more of the negotiated MTU
    else
        chunk_size = (mtu > 23) ? (mtu - 3) : 20;

    if (frame_len > STREAM_SLICE_MAX)
        frame_len = STREAM_SLICE_MAX;

    size_t offset = 0;
    int consecutive_failures = 0;

    while (offset < frame_len) {
        esp_task_wdt_reset();

        size_t remaining = frame_len - offset;
        size_t take = MIN(chunk_size, remaining);

        // Reuse pre-allocated static notify_buf instead of stack buffer
        if (offset == 0)
            memcpy(notify_buf, frame, 8);
        memcpy(notify_buf + 8, frame + offset, take);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(notify_buf, 8 + take);

        if (!om) {
            vTaskDelay(pdMS_TO_TICKS(3)); // back off slightly
            if (++consecutive_failures >= 5)
                return -1;
            continue;
        }

        int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
        if (rc != 0) {
            os_mbuf_free_chain(om);
            vTaskDelay(pdMS_TO_TICKS(5));
            if (++consecutive_failures >= 5)
                return -1;
            continue;
        }

        offset += take;
        consecutive_failures = 0;

        if (rc == 0) {
            ble_backoff_ms = MAX(2, ble_backoff_ms - 1); // speed up
        } else {
            ble_backoff_ms = MIN(50, ble_backoff_ms + 5); // slow down
        }
        vTaskDelay(pdMS_TO_TICKS(ble_backoff_ms));
    }

    return 0;
}

static void ble_send_json_response(const char *json_str) {
    if (!atomic_load(&bt_conn) || g_stream_attr_handle == 0) {
        ESP_LOGW("BLE_ACK", "No active BLE connection, cannot send ack");
        return;
    }

    size_t len = strlen(json_str);
    if (len > MAX_NOTIFY_BUF - 1) len = MAX_NOTIFY_BUF - 1;

    memcpy(notify_buf, json_str, len);
    notify_buf[len] = '\0';

    struct os_mbuf *om = ble_hs_mbuf_from_flat(notify_buf, len);
    if (!om) {
        ESP_LOGW("BLE_ACK", "Failed to alloc BLE mbuf for ack");
        return;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, g_stream_attr_handle, om);
    if (rc != 0) {
        ESP_LOGW("BLE_ACK", "Notify failed rc=%d", rc);
    } else {
        ESP_LOGI("BLE_ACK", "Sent BLE ACK: %s", json_str);
    }
}


static int gatt_notify_only_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // No read/write permitted — just return success for compatibility.
    return 0;
}

/* ---------- GATT services ---------- */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFEED),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID128_DECLARE(
                    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x00,0x10,0x00,0x01,0x00,0xed,0xfe
                ),
                .access_cb = gatt_control_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID128_DECLARE(
                    0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,0x00,0x00,0x10,0x00,0x02,0x00,0xed,0xfe
                ),
                .access_cb = gatt_notify_only_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};


/* ---------- BLE GAP events ---------- */
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                g_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "Connected (handle %d)", g_conn_handle);
                
                // Request slightly longer connection interval to reduce radio duty-cycle (saves power/heat)
                struct ble_gap_upd_params params = {
                    .itvl_min = 24,   // 24 * 1.25 ms = 30 ms
                    .itvl_max = 30,   // 30 * 1.25 ms = 37.5 ms
                    .latency  = 4,    // allow some event skipping
                    .supervision_timeout = 400, // 4 s
                };
                int upd_rc = ble_gap_update_params(g_conn_handle, &params);
                ESP_LOGI(TAG, "ble_gap_update_params rc=%d (30-37ms, latency=4)", upd_rc);

                ESP_LOGI(TAG, "ble_gap_set_preferred_phys rc=%d", phy_rc);
                atomic_store(&bt_conn, true);

            } else {
                ESP_LOGW(TAG, "Connect failed; status=%d", event->connect.status);
                // Restart advertising
                ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
            }
            break;

            case BLE_GAP_EVENT_DISCONNECT:
                ESP_LOGW(TAG, "Client disconnected (reason=%d)", event->disconnect.reason);

                // Atomic BLE state reset
                atomic_store(&mic_active, false);
                atomic_store(&bt_conn, false);
                atomic_store(&streaming, false);
                g_stream_attr_handle = 0;

                ESP_LOGI(TAG, "Flushed pending ringbuffer items and reset backoff");

                vTaskDelay(pdMS_TO_TICKS(200));
                ESP_LOGI(TAG, "Restarting advertising after disconnect...");
                ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
                break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "Subscribe: conn=%d attr_handle=%d cur_notify=%d",
                    event->subscribe.conn_handle,
                    event->subscribe.attr_handle,
                    event->subscribe.cur_notify);

            if (event->subscribe.cur_notify) {
                g_stream_attr_handle = event->subscribe.attr_handle;
                ESP_LOGI(TAG, "Cached stream attr handle: %d", g_stream_attr_handle);
            } else if (g_stream_attr_handle == event->subscribe.attr_handle) {
                g_stream_attr_handle = 0;
            }
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU exchange event: conn=%d mtu=%u",
                     event->mtu.conn_handle, event->mtu.value);
            uint16_t cur_mtu = ble_att_mtu(g_conn_handle);
            ESP_LOGI(TAG, "ble_att_mtu() => %u", cur_mtu);
            break;

        default:
            break;
    }

    return 0;
}

/* ---------- Advertising (uses proper uuid type) ---------- */
static void start_advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    const char *name = "Speechster-B1";
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t*) name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    /* Use ble_uuid16_t array for fields.uuids16 */
    static const ble_uuid16_t adv_uuids[] = { BLE_UUID16_INIT(0xFEED) };
    fields.uuids16 = adv_uuids;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &advp, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising started (name=%s)", name);
    }
}

/* NimBLE sync callback */
static void ble_on_sync(void) {
    ble_svc_gap_device_name_set("Speechster-B1");


    // Request preferred ATT MTU
    int rc = ble_att_set_preferred_mtu(517);
    ESP_LOGI(TAG, "ble_att_set_preferred_mtu rc=%d", rc);


    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return;
    }

    rc = ble_gatts_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_start rc=%d", rc);
        return;
    }

    start_advertise();
}


/* NimBLE host task */
static void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Initialize NimBLE */
static void init_nimble(void) {
    esp_err_t rc;

    // NVS must be inited before BT controller
    rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(rc);
    }

    // Initialize nimble host stack
    nimble_port_init();

    // Host callbacks
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;

    // Start the NimBLE host task
    nimble_port_freertos_init(nimble_host_task);
}

// ─────────────────────────────────────────────
// Wi-Fi + Server setup
// ─────────────────────────────────────────────

static httpd_handle_t stream_server = NULL;

static esp_err_t audio_stream_post(httpd_req_t *req) {
    char buf[512];
    int ret, remaining = req->content_len;
    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) break;
        remaining -= ret;
        // Here you can feed the incoming audio to your decoder/processor
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static const httpd_uri_t audio_post = {
    .uri = "/audio",
    .method = HTTP_POST,
    .handler = audio_stream_post,
};

static void start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    if (httpd_start(&stream_server, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_server, &audio_post);
        ESP_LOGI("HTTP", "Audio endpoint started on port %d", config.server_port);
    }
}

// ─────────────────────────────────────────────
// Wi-Fi Credential Persistence (NVS)
// ─────────────────────────────────────────────
static void save_wifi_creds(const char *ssid, const char *pass, const char *ip, const char *port) {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "pass", pass);
        nvs_set_str(nvs, "ip", ip);
        nvs_set_str(nvs, "port", port);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI("WiFiNVS", "Saved Wi-Fi creds: SSID=%s IP=%s PORT=%s", ssid, ip, port);
    } else {
        ESP_LOGE("WiFiNVS", "Failed to open NVS for Wi-Fi save");
    }
}


static bool load_wifi_creds(char *ssid, size_t ssid_len,
                            char *pass, size_t pass_len,
                            char *ip, size_t ip_len,
                            char *port, size_t port_len) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) return false;

    bool ok = true;
    if (nvs_get_str(nvs, "ssid", ssid, &ssid_len) != ESP_OK) ok = false;
    if (nvs_get_str(nvs, "pass", pass, &pass_len) != ESP_OK) ok = false;
    if (nvs_get_str(nvs, "ip", ip, &ip_len) != ESP_OK) ok = false;
    if (nvs_get_str(nvs, "port", port, &port_len) != ESP_OK) ok = false;

    nvs_close(nvs);
    return ok;
}


static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t) handler_args;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            if (client == ws_client_ctrl) {
                ws_ctrl_connected = true;
                ESP_LOGI("WS", "Control WS connected (%p)", client);
            } else if (client == ws_client_audio) {
                ws_audio_connected = true;
                ESP_LOGI("WS", "Audio WS connected (%p)", client);
            } else {
                ESP_LOGI("WS", "Unknown WS connected (%p)", client);
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            if (client == ws_client_ctrl) {
                ws_ctrl_connected = false;
                ESP_LOGW("WS", "Control WS disconnected");
                if (ws_client_ctrl) {
                    esp_websocket_client_destroy(ws_client_ctrl);
                    ws_client_ctrl = NULL;
                }
            } else if (client == ws_client_audio) {
                ws_audio_connected = false;
                ESP_LOGW("WS", "Audio WS disconnected");
                if (ws_client_audio) {
                    esp_websocket_client_destroy(ws_client_audio);
                    ws_client_audio = NULL;
                }
            } else {
                ESP_LOGW("WS", "Unknown WS disconnected");
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            // Only parse JSON on control socket. Audio socket will deliver binary frames we ignore here.
            if (client == ws_client_ctrl) {
                // data->data_ptr is not null-terminated — use ParseWithLength as you did.
                cJSON *root = cJSON_ParseWithLength(data->data_ptr, data->data_len);
                if (root) {
                    cJSON *cmd = cJSON_GetObjectItem(root, "command");
                    if (cmd && cJSON_IsObject(cmd)) {
                        ESP_LOGI("WS", "Received WS command (control socket)");
                        handle_control_json(cmd);
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGW("WS", "Control WS: invalid JSON received");
                }
            } else if (client == ws_client_audio) {
                // Audio frames are arriving here as binary — server writes them to disk. No-op on device.
                // Optionally you can monitor incoming server messages on this socket if your server sends acks.
            }
            break;

        default:
            break;
    }
}

// ─────────────────────────────────────────────
// Wi-Fi event handlers
// ─────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGW(WIFI_TAG, "Disconnected, retrying...");
        if (wifi_http_client) {
            esp_http_client_cleanup(wifi_http_client);
            wifi_http_client = NULL;
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
        ESP_LOGI(WIFI_TAG, "Connected to Wi-Fi");

        // Create / start control WS client
        esp_websocket_client_config_t ctrl_cfg = {
            .uri = WS_CTRL_URI[0] ? WS_CTRL_URI : "ws://0.0.0.0:0/ws",
            .reconnect_timeout_ms = 3000,
            .keep_alive_idle = 10,
            .keep_alive_interval = 10,
            .keep_alive_count = 3
        };
        ws_client_ctrl = esp_websocket_client_init(&ctrl_cfg);
        if (ws_client_ctrl) {
            esp_websocket_register_events(ws_client_ctrl, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void*)ws_client_ctrl);
            esp_websocket_client_start(ws_client_ctrl);
            ESP_LOGI(WIFI_TAG, "Control WS client started (%s)", WS_CTRL_URI);
        } else {
            ESP_LOGW(WIFI_TAG, "Failed to init control WS client");
        }

        // Create / start audio WS client
        esp_websocket_client_config_t audio_cfg = {
            .uri = WS_AUDIO_URI[0] ? WS_AUDIO_URI : "ws://0.0.0.0:0/data/audio",
            .reconnect_timeout_ms = 3000,
            .keep_alive_idle = 10,
            .keep_alive_interval = 10,
            .keep_alive_count = 3
        };
        ws_client_audio = esp_websocket_client_init(&audio_cfg);
        if (ws_client_audio) {
            esp_websocket_register_events(ws_client_audio, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void*)ws_client_audio);
            esp_websocket_client_start(ws_client_audio);
            ESP_LOGI(WIFI_TAG, "Audio WS client started (%s)", WS_AUDIO_URI);
        } else {
            ESP_LOGW(WIFI_TAG, "Failed to init audio WS client");
        }

        ESP_LOGI(WIFI_TAG, "Starting HTTP server");
        if (!stream_server) {
            start_http_server();
        }

    }
}

void wifi_init_sta(const char *ssid, const char *pass) {
    if (esp_event_loop_create_default() != ESP_ERR_INVALID_STATE) {
        // only create if not already present
        esp_netif_init();
        esp_netif_create_default_wifi_sta();
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    // Limit WiFi TX power by default to reduce heat and range (quarter-dBm units)
    esp_err_t rc = esp_wifi_set_max_tx_power(32); // ~10 dBm
    if (rc != ESP_OK) ESP_LOGW("WiFiPM", "esp_wifi_set_max_tx_power rc=%s", esp_err_to_name(rc));


    ESP_LOGI("WiFi", "Connecting to SSID:%s", ssid);
}

// ─────────────────────────────────────────────
// BLE callback to receive Wi-Fi credentials
// ─────────────────────────────────────────────
void ble_on_wifi_creds_received(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    const cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItem(root, "pass");
    const cJSON *j_ip   = cJSON_GetObjectItem(root, "host_ip");
    const cJSON *j_port = cJSON_GetObjectItem(root, "host_port");

    if (!cJSON_IsString(j_ssid) || !cJSON_IsString(j_pass) ||
        !cJSON_IsString(j_ip)   || !cJSON_IsString(j_port)) {
        ESP_LOGE("BLE_CFG", "Invalid BLE JSON received");
        cJSON_Delete(root);
        ble_send_json_response("{\"status\":\"ERR\",\"msg\":\"Invalid JSON\"}");
        return;
    }

    const char *ssid = j_ssid->valuestring;
    const char *pass = j_pass->valuestring;
    const char *ip   = j_ip->valuestring;
    const char *port = j_port->valuestring;

    snprintf(WS_CTRL_URI, sizeof(WS_CTRL_URI), "ws://%s:%s/ws", ip, port);
    snprintf(WS_AUDIO_URI, sizeof(WS_AUDIO_URI), "ws://%s:%s/data/audio", ip, port);
    snprintf(OTA_URL, sizeof(OTA_URL), "http://%s:%s/ota/latest.bin", ip, port);
    snprintf(TELEMETRY_URL, sizeof(TELEMETRY_URL), "http://%s:%s/esp/telemetry", ip, port);


    ESP_LOGI("BLE_CFG", "Parsed BLE JSON: SSID=%s, IP=%s:%s", ssid, ip, port);
    save_wifi_creds(ssid, pass, ip, port);

    // Send BLE ACK immediately
    ble_send_json_response("{\"status\":\"OK\",\"msg\":\"WiFi credentials saved\"}");

    // Proceed with WiFi connection + WebSocket setup
    wifi_init_sta(ssid, pass);

    cJSON_Delete(root);
}

void loadNVSData(void) {
    char ssid[32], pass[64], ip[32], port[16];
    if (load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass), ip, sizeof(ip), port, sizeof(port))) {
        
        snprintf(WS_CTRL_URI, sizeof(WS_CTRL_URI), "ws://%s:%s/ws", ip, port);
        snprintf(WS_AUDIO_URI, sizeof(WS_AUDIO_URI), "ws://%s:%s/data/audio", ip, port);
        snprintf(OTA_URL, sizeof(OTA_URL), "http://%s:%s/ota/latest.bin", ip, port);
        snprintf(TELEMETRY_URL, sizeof(TELEMETRY_URL), "http://%s:%s/esp/telemetry", ip, port);


        ESP_LOGI("WiFiNVS", "Loaded Wi-Fi and Server Info:");
        ESP_LOGI("WiFiNVS", "SSID=%s  IP=%s:%s", ssid, ip, port);
        ESP_LOGI("WiFiNVS", "WS_CTRL_URI=%s", WS_CTRL_URI);

        wifi_init_sta(ssid, pass);
    } else {
        ESP_LOGW("WiFiNVS", "No saved Wi-Fi credentials");
    }
}

static void telemetry_task(void *arg)
{
    while (1) {
        wifi_ap_record_t info;

        ESP_LOGI(TAG, "Telemetry: heap=%u free_min=%u tasks=%u bt=%d ws=%d",
                (unsigned)esp_get_free_heap_size(),
                (unsigned)esp_get_minimum_free_heap_size(),
                uxTaskGetNumberOfTasks(),
                atomic_load(&bt_conn),
                ws_ctrl_connected);

        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK)
            ESP_LOGI(TAG, "RSSI=%d dBm", info.rssi);
        
        if (wifi_send_q) {
            UBaseType_t qfill = uxQueueMessagesWaiting(wifi_send_q);
            ESP_LOGI(TAG, "Telemetry: heap=%u free_min=%u queue=%u ws=%d",
                    (unsigned)esp_get_free_heap_size(),
                    (unsigned)esp_get_minimum_free_heap_size(),
                    (unsigned)qfill, ws_ctrl_connected);

        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static const char *TAG_OTA = "OTA";

void perform_ota_update(void *pvParameters)
{
    ESP_LOGI(TAG_OTA, "Starting OTA update from: %s", OTA_URL);

    esp_http_client_config_t http_cfg = {
        .url = OTA_URL[0] ? OTA_URL : "http://0.0.0.0:0/ota/latest.bin",
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_OTA, "Rebooting in 2s...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGE(TAG_OTA, "OTA failed: %s", esp_err_to_name(ret));
    }

    vTaskDelete(NULL); 
}

static void wifi_watchdog_task(void *arg) {
    while (1) {
        static int wifi_retry = 0;
        if (!wifi_connected) {
            wifi_retry++;
            if (wifi_retry > 5) {
                ESP_LOGE("WiFiWD", "Too many retries — rebooting");
                esp_restart();
            }
            esp_wifi_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void telemetry_push_task(void *arg) {
    while (1) {
        if (!wifi_connected) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }


        float temp = read_temperature_c();
        if (temp < -500.0f) temp = 0.0f; // guard if sensor unavailable

        snprintf(json, sizeof(json),
                "{\"device_id\":\"Speechster_B1\",\"heap\":%u,\"frames_sent\":%" PRIu32 ",\"temp\":%.2f}",
                (unsigned)esp_get_free_heap_size(), frames_sent, temp);

        esp_http_client_config_t cfg = {
            .url = TELEMETRY_URL[0] ? TELEMETRY_URL : "http://0.0.0.0:0/esp/telemetry",
            .method = HTTP_METHOD_POST,
            .timeout_ms = 3000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json, strlen(json));
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);

        vTaskDelay(pdMS_TO_TICKS(10000)); // every 10s
    }
}

/* init on-chip temperature sensor (ESP32-S3) */
static void init_temp_sensor(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT();
    // Set reasonable sample cycles if API requires (default macro used above)
    esp_err_t rc = temperature_sensor_install(&cfg, &temp_sensor_handle);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "temp_sensor install failed: %s", esp_err_to_name(rc));
        temp_sensor_handle = NULL;
        return;
    }
    rc = temperature_sensor_enable(temp_sensor_handle);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "temp_sensor enable failed: %s", esp_err_to_name(rc));
        temperature_sensor_uninstall(temp_sensor_handle);
        temp_sensor_handle = NULL;
        return;
    }
    ESP_LOGI(TAG, "Temp sensor available");
}

/* safe read wrapper, returns -1000 on error */
static float read_temperature_c(void)
{
    if (!temp_sensor_handle) return -1000.0f;
    esp_err_t rc = temperature_sensor_get_celsius(temp_sensor_handle, &current_temp_c);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "temp_sensor read error: %s", esp_err_to_name(rc));
        return -1000.0f;
    }
    return current_temp_c;
}

/* Thermal watchdog — auto-throttle radios and stop audio if needed */
static void thermal_watchdog_task(void *arg)
{
    while (1) {
        float t = read_temperature_c();
        if (t > -100.0f) { // valid reading
            ESP_LOGI("THERMAL", "Internal temp: %.2f °C", t);

            if (t >= THERMAL_CRIT_C) {
                ESP_LOGE("THERMAL", "CRITICAL TEMP %.2f °C — stopping mic and throttling radios", t);
                stop_mic_streaming();
                // Further reduce WiFi TX to minimum (quarter-dBm units): 8 -> ~2 dBm
                esp_err_t rc = esp_wifi_set_max_tx_power(8);
                if (rc != ESP_OK) ESP_LOGW("THERMAL", "esp_wifi_set_max_tx_power rc=%s", esp_err_to_name(rc));
            } else if (t >= THERMAL_WARN_C) {
                ESP_LOGW("THERMAL", "High temp %.2f °C — throttling radios", t);
                // Lower WiFi TX power moderately (e.g. 32 -> ~8 dBm)
                esp_err_t rc = esp_wifi_set_max_tx_power(32);
                if (rc != ESP_OK) ESP_LOGW("THERMAL", "esp_wifi_set_max_tx_power rc=%s", esp_err_to_name(rc));
            } else {
                // Normal operation: allow configured TX power (~40 -> ~10 dBm)
                esp_err_t rc = esp_wifi_set_max_tx_power(40);
                if (rc != ESP_OK) ESP_LOGW("THERMAL", "esp_wifi_set_max_tx_power rc=%s", esp_err_to_name(rc));
            }
        } else {
            ESP_LOGD("THERMAL", "Temp read invalid, skipping thermal actions");
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); // check every 5 s
    }
}

/* ─────────────────────────────────────────────
 *  DIE TEMPERATURE failsafe
 *  Final protection layer: BLE-priority emergency alert + full shutdown
 * ───────────────────────────────────────────── */
static void die_temperature_task(void *arg)
{
    const float DIE_TEMP_C = 75.0f;  // Persistent threshold
    static uint64_t above_start_ms = 0;

    while (1) {
        float t = read_temperature_c();
        if (t > -100.0f) {
            if (t >= DIE_TEMP_C) {
                uint64_t now = esp_timer_get_time() / 1000ULL;
                if (above_start_ms == 0)
                    above_start_ms = now;

                if ((now - above_start_ms) >= 1000) {  // ≥1s sustained
                    ESP_LOGE("DIE_TEMP", "🔥 DIE TEMPERATURE REACHED (%.2f °C)", t);
                    ESP_LOGE("DIE_TEMP", "Shutting down all subsystems...");

                    // 1️⃣ Kill active tasks first
                    stop_mic_streaming();
                    adc_stop();

                    // 2️⃣ Compose emergency message
                    const char *die_msg = "{\"msg\":\"Die Temps Reached\"}";

                    // 3️⃣ BLE first — must reach doctor ASAP
                    ble_send_json_response(die_msg);

                    // 4️⃣ Then Wi-Fi to backend for logging
                    if (wifi_connected) {
                        esp_http_client_config_t cfg = {
                            .url = TELEMETRY_URL[0] ? TELEMETRY_URL : "http://0.0.0.0:0/esp/telemetry",
                            .method = HTTP_METHOD_POST,
                            .timeout_ms = 2000,
                        };
                        esp_http_client_handle_t c = esp_http_client_init(&cfg);
                        esp_http_client_set_header(c, "Content-Type", "application/json");
                        esp_http_client_set_post_field(c, die_msg, strlen(die_msg));
                        esp_http_client_perform(c);
                        esp_http_client_cleanup(c);
                    }

                    // 5️⃣ Disable everything except radios
                    esp_wifi_stop();     // optionally stop Wi-Fi
                    esp_bt_controller_disable(); // optional, leave if BLE alert confirmed

                    // 6️⃣ Log and sleep forever
                    ESP_LOGE("DIE_TEMP", "System entering deep shutdown to prevent damage.");
                    vTaskDelay(pdMS_TO_TICKS(100)); // small delay to flush UART

                    // Disable all wake sources to make shutdown permanent until power-cycle
                    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
                    esp_deep_sleep_start();  // full power-off
                }
            } else {
                above_start_ms = 0;  // reset counter once cooled
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200)); // poll 5×/s for responsiveness
    }
}


/* ----------------------- PM: helper to enable dynamic scaling (call once) ----------------------- */
static void setup_power_management(void)
{
#ifdef CONFIG_PM_ENABLE
    // On S3 the esp_pm_config_esp32s3_t struct is available; falling back to esp_pm_config_t works on many IDF versions.
    // We'll use esp_pm_config_t as in your existing code but set light sleep enabled.
    esp_pm_config_esp32s3_t pmcfg = {
        .max_freq_mhz = SPEECHSTER_PM_MAX_MHZ,
        .min_freq_mhz = SPEECHSTER_PM_MIN_MHZ,
        .light_sleep_enable = true
    };
    esp_err_t rc = esp_pm_configure(&pmcfg);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(rc));
    } else {
        ESP_LOGI(TAG, "PM configured %u-%u MHz, light sleep enabled", SPEECHSTER_PM_MIN_MHZ, SPEECHSTER_PM_MAX_MHZ);
    }
#else
    ESP_LOGW(TAG, "Power management not enabled in sdkconfig");
#endif
}

/* ---------- app_main ---------- */
void app_main(void) {
    esp_log_level_set("NimBLE", ESP_LOG_ERROR);
    //esp_log_level_set("*", ESP_LOG_NONE);
    ESP_LOGI(TAG, "Speechster B1 Starting Up");

    state_lock = xSemaphoreCreateMutex();

    ringbuf_mutex = xSemaphoreCreateMutex();
    if (!ringbuf_mutex) {
        ESP_LOGE(TAG, "Failed to create ringbuf_mutex");
        return;
    }

    // I2S Init & Config
    i2s_init_inmp441();

    const uint32_t prealloc_frame_ms = 10;
    const size_t prealloc_frame_samples = (AUDIO_SAMPLE_RATE / 1000) * prealloc_frame_ms;
    const size_t prealloc_bytes_per_read = prealloc_frame_samples * sizeof(int32_t);
    const size_t prealloc_pcm_bytes = prealloc_frame_samples * sizeof(int16_t);

    i2s_dma_buf = heap_caps_malloc(prealloc_bytes_per_read, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    pcm_dma_buf = heap_caps_malloc(prealloc_pcm_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!i2s_dma_buf || !pcm_dma_buf) {
        ESP_LOGE(TAG, "Failed to alloc I2S DMA buffers (size %u / %u)", (unsigned)prealloc_bytes_per_read, (unsigned)prealloc_pcm_bytes);
        // handle failure: either abort startup or fall back to non-DMA (not recommended)
        // For now, abort.
        return;
    }
    memset(i2s_dma_buf, 0, prealloc_bytes_per_read);
    memset(pcm_dma_buf, 0, prealloc_pcm_bytes);

    // Start ctrl_queue early
    ctrl_queue = xQueueCreate(CTRL_QUEUE_LEN, sizeof(control_msg_t));

    if (!ctrl_queue) {
        ESP_LOGE(TAG, "Failed to create ctrl_queue");
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    #ifdef CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = false
    };
    esp_pm_configure(&pm_config);
    #endif

    setup_power_management();
    init_temp_sensor();

    // Start Thermal Watchdog Task before anything else
    xTaskCreatePinnedToCore(thermal_watchdog_task, "thermal_wd", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(die_temperature_task, "die_temp", 4096, NULL, 5, NULL, 1);

    // Start ctrl_task Early
    xTaskCreatePinnedToCore(control_task, "ctrl_task", 6144, NULL, 5, NULL, 1);
    xTaskCreate(wifi_watchdog_task, "wifi_wdt_task", 2048, NULL, 2, NULL);

    // Init NimBLE
    init_nimble();
    
    // Load NVS Data
    loadNVSData();

    static uint8_t agg_buf_static[8 + AGGREGATE_BYTES + 4];
    agg_buf = agg_buf_static;
    memset(agg_buf, 0, sizeof(agg_buf_static));

    vTaskDelay(pdMS_TO_TICKS(200));  // let system timers, NimBLE, heap settle

    ESP_LOGI(TAG, "rx_chan ptr=%p", rx_chan);
    if (rx_chan == NULL) {
        ESP_LOGE(TAG, "I2S channel invalid!");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Start Tasks

    // ─────────────────────────────────────────────
    // Start Wi-Fi send queue and task
    // ─────────────────────────────────────────────
    wifi_send_q = xQueueCreate(WIFI_SEND_QUEUE_LEN, sizeof(wifi_frame_t));
    if (!wifi_send_q) {
        ESP_LOGE("WiFiSendTask", "Failed to create Wi-Fi send queue");
    } else {
        xTaskCreatePinnedToCore(wifi_send_task, "wifi_send", 8192, NULL, 4, NULL, 1);
    }

    xTaskCreatePinnedToCore(i2s_capture_task, "i2s_cap", 16*1024, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(pSensor_task, "pSensor", 4*1024, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(telemetry_task, "telemetry", 4*1024, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(telemetry_push_task, "telemetry_push", 4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "Main started; advertising automatically, UART logs enabled");
    ESP_LOGI(TAG, "Build %s (%s) — IDF %s", __DATE__, __TIME__, esp_get_idf_version());

    vTaskDelay(pdMS_TO_TICKS(100));  // let tasks start
}