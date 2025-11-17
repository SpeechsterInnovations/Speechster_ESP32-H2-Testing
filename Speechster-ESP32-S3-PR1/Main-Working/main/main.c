// BUILD: 1.6.0-rc-alpha
// ─────────────────────────────────────────────
// Speechster-B1 v1.6 RELEASE CANDIDATE
// Nov 2025 — "The one that might actually work"
// Safe aggregation, adaptive BLE pacing, heap-based buffers
// Built for Espressif's ESP32-S3-DevKitC-1-N8R2 Development Board
// ─────────────────────────────────────────────

/* “This is the world’s first digital tongue depressor — one that listens, measures, and learns.”

    I can basically wow them by showing:

    The problem clearly — “Doctors currently rely on visual observation and subjective judgment for speech & oral motor issues.”
    The solution — “Our device digitizes that process using sensors and AI.”
    The demo — “Here’s the device capturing real-time speech data and sending it to an AI that interprets it.”
    The impact — “Faster diagnosis, objective tracking, and potential for telemedicine or early detection.”
*/

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
#include "esp_rom_sys.h"
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
#include "driver/gpio.h"

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
#include "freertos/semphr.h"
#include "esp_websocket_client.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"

// PM
#include "driver/temperature_sensor.h"   // on-chip temp sensor (ESP32-S3)
#include "esp_sleep.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

/* Brownout Protection (To prevent huge WiFi/BLE bursts) */
#include "soc/soc_caps.h"
#include "esp_err.h"

static const char *TAG = "speechster_b1";

/* ---------- Defaults & config ---------- */
#define UART_BAUD_RATE       115200
#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_FRAME_MS       10
#define AUDIO_FRAME_SAMPLES   ((AUDIO_SAMPLE_RATE / 1000) * AUDIO_FRAME_MS)
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_READ_BYTES     (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_RINGBUF_SIZE   (48 * 1024)
#define STREAM_SLICE_MAX   (8 * 1024) // Smaller slice size reduces total number of chunks per logical frame
#define I2S_BUF_SIZE 2048
#define AGGREGATE_BYTES  (15 * 320)
#define LOG_CTRL(fmt, ...) ESP_LOGI(TAG, "[CTRL_IN] " fmt, ##__VA_ARGS__)
#define LOG_JSON_PARSE(fmt, ...) ESP_LOGI(TAG, "[JSON_PARSE] " fmt, ##__VA_ARGS__)
#define HEAP_TRACE_RECORDS 256
#define ENABLE_TELEMETRY 0

#define WIFI_SEND_QUEUE_LEN 12
static const size_t WS_SEND_CHUNK          = 512;   // larger chunk -> fewer send calls
static const int    MAX_SEND_RETRIES       = 3;     // attempts per chunk
static const int    BASE_TIMEOUT_MS        = 300;   // ws send timeout
static const int    MAX_REQUEUE_PER_FRAME  = 2;     // allow a couple requeues
static const int    DESTROY_ENQUEUE_TMO_MS = 10;
static const int    POST_DESTROY_WAIT_MS   = 200;   // breathing room after manager processes destroy

/* ---------- Heap-safe pre-allocated buffers ---------- */
#define FRAMES_PER_BATCH      3         // aggressive low-latency: 3 × 10ms = 30ms per batch (much lower E2E latency)
#define MAX_BATCH_PCM_BYTES  ( ((AUDIO_SAMPLE_RATE/1000) * AUDIO_FRAME_MS) * sizeof(int16_t) * FRAMES_PER_BATCH )
#define MAX_FRAME_SIZE       (8 + MAX_BATCH_PCM_BYTES)
static uint8_t audio_frame_buf[MAX_FRAME_SIZE];

static i2s_chan_handle_t rx_chan = NULL; 
static uint32_t rb_overflow_count = 0;
static uint32_t ble_delay_ms = 10;

#define I2S_PORT_NUM         I2S_NUM_0
#define I2S_BCK_IO           2   // SCK
#define I2S_WS_IO            3   // WS / LRCLK
#define I2S_DATA_IN_IO       5   // SD (data from mic)
#define SAMPLE_BUF_LEN       320  // Number of samples per read

// Top-level globals
static int32_t *i2s_dma_buf = NULL;
static adc_continuous_handle_t adc_handle = NULL;
static QueueHandle_t adc_evt_queue = NULL;
static bool adc_running = false;

static temperature_sensor_handle_t temp_sensor_handle = NULL;
static float current_temp_c = 0.0f;

typedef enum {
    WS_ACTION_NONE = 0,
    WS_ACTION_STOP_CTRL,
    WS_ACTION_DESTROY_CTRL,
    WS_ACTION_STOP_AUDIO,
    WS_ACTION_DESTROY_AUDIO,
} ws_action_t;

static QueueHandle_t ws_mgr_q = NULL;


/* PM config (tunable) */
#ifndef SPEECHSTER_PM_MAX_MHZ
#define SPEECHSTER_PM_MAX_MHZ 160
#endif
#ifndef SPEECHSTER_PM_MIN_MHZ
#define SPEECHSTER_PM_MIN_MHZ 80
#endif

/* Thermal thresholds (adjust to your enclosure / testing) */
#define THERMAL_WARN_C   70.0f   // throttling threshold
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

typedef struct {
    char ssid[32];
    char pass[64];
    char ip[32];
    char port[16];
} wifi_creds_t;

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
static volatile uint64_t idle_ticks = 0;
static SemaphoreHandle_t ringbuf_mutex = NULL; // Protects ringbuffer access without disabling interrupts
static atomic_bool wifi_connected = ATOMIC_VAR_INIT(false);
static atomic_bool ws_ctrl_connected = ATOMIC_VAR_INIT(false);
static atomic_bool ws_audio_connected = ATOMIC_VAR_INIT(false);
static atomic_bool wifi_wdt_active = ATOMIC_VAR_INIT(false);
static uint32_t frames_dropped = 0;
static _Atomic bool mic_streaming_active = false;

// new: request that capture task deinit i2s channel
static atomic_bool i2s_deinit_requested = ATOMIC_VAR_INIT(false);
// and small helper to test if rx_chan exists atomically
static inline bool rx_chan_ready(void) {
    return rx_chan != NULL;
}

static SemaphoreHandle_t ws_audio_lock = NULL;



/* Set true once esp_wifi_start() returned ESP_OK (soft indicator) */
static volatile bool wifi_stack_started = false;
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

// Forward declarations
static int send_chunked_notify(uint16_t conn_handle,
                               uint16_t attr_handle,
                               const uint8_t *frame,
                               size_t frame_len);

static bool start_mic_streaming(void);
static void stop_mic_streaming(void);
void perform_ota_update(void *pvParameters);
static void ble_send_json_response(const char *json_str);
static float read_temperature_c(void);
void ble_on_wifi_creds_received(const char *json_str);
static esp_netif_t* ensure_sta_netif(void);

/* ------------------- WiFi Config ------------------- */
static const char *WIFI_TAG = "WiFiSend";

// Dynamic server URIs — filled at runtime from BLE credentials or NVS
static char WS_CTRL_URI[128]  = {0};   // ws://<host>:<port>/ws    (control + telemetry)
static char WS_AUDIO_URI[128] = {0};   // ws://<host>:<port>/data/audio  (binary audio)
static char OTA_URL[128]      = {0};
static char TELEMETRY_URL[128]= {0};

// Two websocket clients: control and audio
static esp_websocket_client_handle_t ws_client_ctrl  = NULL;
static esp_websocket_client_handle_t ws_client_audio = NULL;

// Per-client connected flags
static esp_http_client_handle_t wifi_http_client = NULL;

typedef struct {
    size_t len;
    uint8_t *data;  // dynamically allocated buffer
} wifi_frame_t;

static QueueHandle_t wifi_send_q = NULL;

// Allocate safe frame object
static wifi_frame_t *alloc_wifi_frame(size_t len) {
    wifi_frame_t *f = heap_caps_malloc(sizeof(wifi_frame_t), MALLOC_CAP_INTERNAL);
    if (!f) return NULL;
    f->data = heap_caps_malloc(len, MALLOC_CAP_INTERNAL);
    if (!f->data) {
        free(f);
        return NULL;
    }
    f->len = len;
    return f;
}

static void free_wifi_frame(wifi_frame_t *f) {
    if (!f) return;
    if (f->data) free(f->data);
    free(f);
}

static void wifi_init_task(void *pv) {

    ESP_LOGI("WiFiTask", "wifi_init_task() running...");

    wifi_creds_t creds;
    if (!pv) {
        vTaskDelete(NULL);
        return;
    }
    // If called with direct creds pointer, use it and free if needed
    memcpy(&creds, pv, sizeof(creds));
    // if pv came from heap, free it here (not used in this sample)

    // *** Do the full wifi init here. Robust logging at each step. ***
    ESP_LOGI("WiFiTask", "Starting wifi_init_sta task for SSID='%s'", creds.ssid);

    esp_err_t err;
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE("WiFiTask", "esp_netif_init failed: %s", esp_err_to_name(err));
        goto done;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("WiFiTask", "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        goto done;
    }

    esp_netif_t *sta_netif = ensure_sta_netif();
    if (!sta_netif) {
        ESP_LOGE("WiFiTask", "ensure_sta_netif returned NULL");
        goto done;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE("WiFiTask", "esp_wifi_init failed: %s", esp_err_to_name(err));
        goto done;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE("WiFiTask", "esp_event_handler_register(WIFI_EVENT) failed: %s", esp_err_to_name(err));
        goto done;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE("WiFiTask", "esp_event_handler_register(IP_EVENT) failed: %s", esp_err_to_name(err));
        goto done;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, creds.ssid, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char *)wifi_config.sta.password, creds.pass, sizeof(wifi_config.sta.password)-1);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE("WiFiTask", "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        goto done;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE("WiFiTask", "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        goto done;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE("WiFiTask", "esp_wifi_start failed: %s", esp_err_to_name(err));
        goto done;
    }
    wifi_stack_started = true;
    ESP_LOGI("WiFiTask", "esp_wifi_start OK");

done:
    // free the heap credentials passed in by caller (if any)
    if (pv) {
        free(pv);
    }

    esp_wifi_set_max_tx_power(40);  // 40 * 0.25dBm = 10 dBm (~10mW)
    if (!ws_audio_lock) ws_audio_lock = xSemaphoreCreateMutex();

    // one-shot task; exit
    vTaskDelete(NULL);

}


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


/* ---------- Helper: ensure default STA netif (avoid double create) ---------- */
static esp_netif_t* ensure_sta_netif(void)
{
    // The default ifkey name used by esp-netif for STA is "WIFI_STA_DEF"
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        ESP_LOGI("WiFi", "Default STA netif already exists -> reusing");
        return netif;
    }

    netif = esp_netif_create_default_wifi_sta();
    if (!netif) {
        ESP_LOGE("WiFi", "esp_netif_create_default_wifi_sta returned NULL");
    } else {
        ESP_LOGI("WiFi", "Created default STA netif");
    }
    return netif;
}

/* Small task to reconfigure Wi-Fi when stack is already started.
 * Argument: pointer to heap-allocated wifi_creds_t (freed here)
 */
static void wifi_reconfigure_task(void *pv)
{
    wifi_creds_t *creds = (wifi_creds_t *)pv;
    if (!creds) vTaskDelete(NULL);

    wifi_mode_t mode;
    wifi_config_t current_conf;
    esp_wifi_get_mode(&mode);
    esp_wifi_get_config(WIFI_IF_STA, &current_conf);

    // Compare new creds against currently configured SSID
    if (strcmp((char *)current_conf.sta.ssid, creds->ssid) == 0) {
        ESP_LOGW("WiFiCFG", "Already connected to the same network — skipping reconfig");
        free(creds);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI("WiFi", "Reconfiguring Wi-Fi with new creds (safe path)");

    // If driver not initialized, bail (caller should create wifi_init_task instead)
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW("WiFi", "esp_wifi not initialized; cannot reconfigure");
        free(creds);
        vTaskDelete(NULL);
        return;
    }


    ESP_LOGI("WiFi", "Reconfiguring Wi-Fi with new creds (safe path)");
    // If driver not initialized, bail (caller should create wifi_init_task instead)
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW("WiFi", "esp_wifi not initialized; cannot reconfigure");
        free(creds);
        vTaskDelete(NULL);
        return;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, creds->ssid, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char *)wifi_config.sta.password, creds->pass, sizeof(wifi_config.sta.password)-1);

    ESP_LOGI("WiFi", "Reconfiguring Wi-Fi — restarting driver to ensure clean state");

    // Force stop/start (safe even if already running)
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_stop();

    while (esp_wifi_stop() == ESP_ERR_WIFI_NOT_INIT) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }



    vTaskDelay(pdMS_TO_TICKS(300));

    esp_err_t rc = esp_wifi_start();
    if (rc == ESP_OK) {
        ESP_LOGI("WiFi", "esp_wifi_start after reconfig OK");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
    } else {
        ESP_LOGE("WiFi", "esp_wifi_start failed after reconfig: %s", esp_err_to_name(rc));
    }


    free(creds);
    vTaskDelete(NULL);
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

static TaskHandle_t i2s_task_handle = NULL;

// Notify wrappers (safe from any context)
static inline void notify_i2s_start(void) {
    if (i2s_task_handle) xTaskNotifyGive(i2s_task_handle);
}

static inline void notify_i2s_stop(void) {
    if (i2s_task_handle) {
        xTaskNotify(i2s_task_handle, 1, eSetBits);  // optional flag
    }
}

static void dynamic_pm_task(void *arg)
{
#ifdef CONFIG_PM_ENABLE
    const int FAST = SPEECHSTER_PM_MAX_MHZ;
    const int SLOW = SPEECHSTER_PM_MIN_MHZ;
    /* Start in low-power idle mode; go FAST when busy */
    int current = SLOW;
    
    while (1) {
        bool busy = atomic_load(&mic_active) || atomic_load(&streaming) ||
                    atomic_load(&bt_conn) || atomic_load(&ws_audio_connected);

        int target = busy ? FAST : SLOW;
        if (target != current) {
            esp_pm_config_t cfg = {
                .max_freq_mhz = target,
                .min_freq_mhz = SLOW,
                .light_sleep_enable = true
            };
            esp_err_t rc = esp_pm_configure(&cfg);
            if (rc == ESP_OK) {
                current = target;
                ESP_LOGI("PM_ADAPT", "CPU freq -> %d MHz (%s mode)",
                         target, busy ? "active" : "idle");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
#else
    vTaskDelete(NULL);
#endif
}


/* ---------- I2S (INMP441) ---------- */
static void i2s_init_inmp441(void)
{
    esp_err_t rc;

    if (rx_chan) {
        ESP_LOGW("I2S", "rx_chan already exists — skipping reinit");
        return;
    }

    // ─────────────────────────────────────────────
    // CHANNEL CONFIG
    // ─────────────────────────────────────────────
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.id = I2S_PORT_NUM;

    i2s_chan_handle_t tx_chan = NULL;
    i2s_chan_handle_t rx_chan_local = NULL;

    rc = i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan_local);
    rx_chan = rx_chan_local;   // assign global

    if (rc != ESP_OK) {
        ESP_LOGE("I2S", "i2s_new_channel failed: %s", esp_err_to_name(rc));
        rx_chan = NULL;
        return;
    }

    // ─────────────────────────────────────────────
    // STD MODE CONFIG
    // ─────────────────────────────────────────────
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_PLL_160M,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = 1,
            .ws_pol = false,
            .bit_shift = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws   = I2S_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DATA_IN_IO,
        },
    };

    rc = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE("I2S", "i2s_channel_init_std_mode failed: %s", esp_err_to_name(rc));
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return;
    }

    // ─────────────────────────────────────────────
    // ENABLE + WARM-UP READ TO FORCE CLOCKS
    // ─────────────────────────────────────────────
    rc = i2s_channel_enable(rx_chan);
    if (rc != ESP_OK) {
        ESP_LOGE("I2S", "i2s_channel_enable failed: %s", esp_err_to_name(rc));
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return;
    }

    // Force BCLK/WS to start toggling.
    uint32_t trash_buf[32];
    size_t br = 0;

    for (int i = 0; i < 8; i++) {  
        i2s_channel_read(rx_chan, trash_buf, sizeof(trash_buf), &br, 10);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // ─────────────────────────────────────────────
    // NOW the pins actually have signal.
    // ─────────────────────────────────────────────
    ESP_LOGI("I2S_DBG", "PinLevels: BCK=%d WS=%d DIN=%d",
             gpio_get_level(I2S_BCK_IO),
             gpio_get_level(I2S_WS_IO),
             gpio_get_level(I2S_DATA_IN_IO));

    ESP_LOGI("I2S", "INMP441 initialized (%u Hz, 32-bit mono L)", AUDIO_SAMPLE_RATE);
}

static void dump_ws_state(const char *ctx) {
    ESP_LOGI("WS_STATE", "%s: ws_client_ctrl=%p ws_client_audio=%p ctrl_conn=%d audio_conn=%d free_heap=%u",
             ctx, ws_client_ctrl, ws_client_audio, (int)atomic_load(&ws_ctrl_connected), (int)atomic_load(&ws_audio_connected),
             (unsigned)esp_get_free_heap_size());
}


// ─────────────────────────────────────────────
// Unified JSON control handler
// ─────────────────────────────────────────────
static void handle_control_json(cJSON *root) {
    if (!root) return;

    // BLE priority: if BLE is connected, ignore WebSocket unless BLE is idle
    if (atomic_load(&bt_conn)) {
        ESP_LOGI(TAG, "BLE connected — BLE commands take priority.");
    } else if (atomic_load(&ws_ctrl_connected)) {
        ESP_LOGI(TAG, "No BLE connection — accepting WebSocket command.");
    } else {
        ESP_LOGW(TAG, "No control channel active; ignoring JSON command.");
        return;
    }

    xSemaphoreTake(state_lock, portMAX_DELAY);

    // Detect BLE Wi-Fi credentials JSON (ssid+pass+host_ip+host_port)
    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass = cJSON_GetObjectItem(root, "pass");
    cJSON *j_ip   = cJSON_GetObjectItem(root, "host_ip");
    cJSON *j_port = cJSON_GetObjectItem(root, "host_port");

    if (j_ssid && j_pass && j_ip && j_port &&
        cJSON_IsString(j_ssid) && cJSON_IsString(j_pass) &&
        cJSON_IsString(j_ip) && cJSON_IsString(j_port)) {
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            ESP_LOGI("CTRL_JSON", "Detected Wi-Fi creds JSON, forwarding to BLE_CFG handler");
            xSemaphoreGive(state_lock);         // <-- release before calling out
            ble_on_wifi_creds_received(json_str);
            free(json_str);
        } else {
            xSemaphoreGive(state_lock);         // ensure we don't keep lock if Print failed
        }
        return; // stop further handling
    }


    cJSON *jmic = cJSON_GetObjectItem(root, "mic");
    if (jmic && cJSON_IsBool(jmic)) {
        if (cJSON_IsTrue(jmic)) {
            vTaskDelay(pdMS_TO_TICKS(300));
            start_mic_streaming();
            vTaskDelay(pdMS_TO_TICKS(10)); // let scheduler breathe
        } else {
            stop_mic_streaming();
            xQueueReset(wifi_send_q); // Instantly clear zombie frames
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

    esp_err_t e = i2s_channel_enable(rx_chan);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX channel: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "RX channel enabled for mic test");
    }


    while (1) {

    if (!rx_chan) {
        ESP_LOGW("DBG", "rx_chan not ready yet, waiting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
    }


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

// NOTE: call this ONLY from i2s_capture_task context.
static void i2s_del_channel_from_capture(void)
{
    if (rx_chan) {
        ESP_LOGI("I2S", "Capture task performing i2s del channel");
        // try to disable the channel first (safe if already disabled)
        esp_err_t rc = i2s_channel_disable(rx_chan);
        if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
            ESP_LOGW("I2S", "i2s_channel_disable rc=%s", esp_err_to_name(rc));
        }
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }
}

static bool wait_for_audio_ws_ready(TickType_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    while (xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout_ms)) {
        if (ws_client_audio && esp_websocket_client_is_connected(ws_client_audio) && atomic_load(&ws_audio_connected)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static bool start_mic_streaming(void) {
    // Quick pre-check: audio WS ready?
    if (!wait_for_audio_ws_ready(500)) {
        ESP_LOGW(TAG, "Audio WS not ready after 500ms — abort mic start");
        return false;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&mic_active, &expected, true)) {
        ESP_LOGW(TAG, "Mic already active");
        return false;
    }

    ESP_LOGI(TAG, "Starting mic streaming...");
    atomic_store(&streaming, true);
    atomic_store(&i2s_deinit_requested, false);
    atomic_store(&mic_streaming_active, true);

    // Always recreate RX channel fresh
    if (rx_chan) {
        ESP_LOGW(TAG, "Destroying stale I2S channel before reinit");
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }

    i2s_init_inmp441();

    if (!rx_chan) {
        ESP_LOGE(TAG, "I2S init failed; aborting mic start");
        atomic_store(&mic_active, false);
        atomic_store(&streaming, false);
        return false;
    }

    notify_i2s_start();
    return true;
}

static void stop_mic_streaming(void) {
    ESP_LOGI(TAG, "Stopping mic streaming...");
    atomic_store(&streaming, false);
    atomic_store(&mic_active, false);
    atomic_store(&mic_streaming_active, false);

    // Request capture task to stop cleanly and deinit the I2S channel when safe.
    atomic_store(&i2s_deinit_requested, true);
    notify_i2s_stop();

    // Optional: wait briefly for i2s_capture_task to complete its shutdown.
    // Do not block too long in case this is called from BLE context; use short timeout.
    for (int i = 0; i < 50; ++i) { // ~50 * 10 ms = 500 ms max
        if (!rx_chan) break; // capture task finished and cleared rx_chan
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (rx_chan) {
        ESP_LOGW("I2S", "stop_mic_streaming timeout waiting for capture task to deinit; leaving rx_chan (it will be cleaned later)");
    } else {
        ESP_LOGI("I2S", "I2S channel deinitialized cleanly");
    }
}

/*
 * Key traits:
 *  - never stops/destroys ws_client_audio from inside the send loop directly
 *  - uses ws_mgr_q to request DESTROY (centralized cleanup)
 *  - robust per-frame requeue + exponential backoff for repeated failures
 *  - short non-blocking lock attempts and clear state checks
 *  - configurable constants at top for tuning
 */


static const char *TAG_WiFi = "WiFiSendTask";

/* small helper request destroy via ws_mgr */
static void request_audio_ws_destroy(void)
{
    if (!ws_mgr_q) {
        ESP_LOGW(TAG_WiFi, "ws_mgr_q not initialized - cannot request destroy");
        return;
    }
    ws_action_t action = WS_ACTION_DESTROY_AUDIO;
    if (xQueueSend(ws_mgr_q, &action, pdMS_TO_TICKS(DESTROY_ENQUEUE_TMO_MS)) != pdTRUE) {
        ESP_LOGW(TAG_WiFi, "ws_mgr_q full - failed to enqueue destroy request");
    } else {
        ESP_LOGI(TAG_WiFi, "Enqueued WS destroy request to ws_mgr_task");
    }
}

/* Small helper to check whether audio WS is usable */
static inline bool audio_ws_ready(void)
{
    if (!ws_client_audio) return false;
    if (!atomic_load(&ws_audio_connected)) return false;
    // try to take the lock briefly to ensure internal client not being destroyed
    if (xSemaphoreTake(ws_audio_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        // busy — treat as temporarily not ready
        return false;
    }
    xSemaphoreGive(ws_audio_lock);
    return true;
}

/* Exponential backoff helper (ms) */
static inline uint32_t calc_backoff_ms(int attempts)
{
    // attempts: 0 -> small wait, 1 -> longer, etc.
    uint32_t base = 50;
    uint32_t backoff = base * (1u << (attempts > 6 ? 6 : attempts));
    if (backoff > 2000) backoff = 2000;
    return backoff;
}

void wifi_send_task(void *arg)
{
    wifi_frame_t *frame = NULL;
    ESP_LOGI(TAG_WiFi, "wifi_send_task start");

    // Protect against spinning requeues for the same frame
    int requeue_count = 0;

    for (;;) {
        
        if (!atomic_load(&mic_streaming_active)) {
            free_wifi_frame(frame);
            vTaskDelay(10);
            continue;
        }

        
        // block until a frame is available
        if (xQueueReceive(wifi_send_q, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!frame || !frame->data || frame->len == 0) {
            free_wifi_frame(frame);
            frame = NULL;
            continue;
        }

        // If mic is off, drop incoming frames instantly
        if (!mic_streaming_active) {
            free_wifi_frame(frame);
            frame = NULL;
            continue;
        }


        // If transport clearly not ready, try requeue with backoff or drop
        if (!audio_ws_ready()) {
            frames_dropped++;
            ESP_LOGW(TAG_WiFi, "audio ws not ready before send — frames_dropped=%u", frames_dropped);

            // Attempt limited requeue with backoff
            if (requeue_count < MAX_REQUEUE_PER_FRAME) {
                requeue_count++;
                uint32_t wait_ms = calc_backoff_ms(requeue_count);
                if (xQueueSend(wifi_send_q, &frame, pdMS_TO_TICKS(200)) == pdTRUE) {
                    ESP_LOGI(TAG, "temporarily requeued frame (try=%d) after %ums", requeue_count, wait_ms);
                    frame = NULL; // ownership transferred
                    vTaskDelay(pdMS_TO_TICKS(wait_ms));
                    continue;
                }
            }

            ESP_LOGW(TAG, "dropping frame (audio not ready and requeue exhausted)");
            free_wifi_frame(frame);
            frame = NULL;
            requeue_count = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Reset per-frame counters
        size_t total = frame->len;
        size_t offset = 0;
        int consecutive_failures = 0;
        requeue_count = 0;

        // Per-frame loop: send in chunks, but avoid blocking long
        while (offset < total) {

            // If mic turned off mid-frame, abort immediately.
            if (!mic_streaming_active) {
                free_wifi_frame(frame);
                frame = NULL;
                break;  // abort chunking for this frame and continue outer loop
            }


            vTaskDelay(pdMS_TO_TICKS(1)); // yield

            size_t remaining = total - offset;
            size_t take = (remaining > WS_SEND_CHUNK) ? WS_SEND_CHUNK : remaining;

            bool chunk_sent = false;
            esp_err_t rc = ESP_FAIL;

            for (int attempt = 0; attempt < MAX_SEND_RETRIES; ++attempt) {
                // quick guard: if manager has requested destroy, abort and requeue/drop
                if (!audio_ws_ready()) {
                    rc = ESP_ERR_INVALID_STATE;
                    break; // do not block trying to send
                }

                // Acquire lock to safely call send
                if (xSemaphoreTake(ws_audio_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
                    // busy — small backoff and retry
                    vTaskDelay(pdMS_TO_TICKS(2 + attempt*2));
                    continue;
                }

                // Re-check connection under lock
                if (!ws_client_audio || !esp_websocket_client_is_connected(ws_client_audio)) {
                    xSemaphoreGive(ws_audio_lock);
                    rc = ESP_ERR_INVALID_STATE;
                    break;
                }

                // Attempt send
                int send_rc = esp_websocket_client_send_bin(ws_client_audio,
                                                        (const char *)(frame->data + offset),
                                                        take,
                                                        BASE_TIMEOUT_MS);

                // Always release the lock after the call
                if (send_rc >= 0) {
                    rc = ESP_OK;           // normalize for downstream logic
                    chunk_sent = true;
                    xSemaphoreGive(ws_audio_lock);
                    break;
                } else {
                    rc = send_rc;          // keep the negative error for decision logic/logging
                    xSemaphoreGive(ws_audio_lock);
                }

                // small jittered backoff before next attempt
                vTaskDelay(pdMS_TO_TICKS(5 + attempt*4));
            }

            if (chunk_sent) {
                offset += take;
                consecutive_failures = 0;
                // gentle throttle so we don't monopolize CPU
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }

            // failed to send the chunk after attempts
            consecutive_failures++;
            frames_dropped++;
            ESP_LOGW(TAG, "chunk send failed at off=%u (consec=%d) rc=%d",
                    (unsigned)offset, consecutive_failures, (int)rc);


            // If transient invalid state, request manager to destroy & rebuild connection and requeue
            if (rc == ESP_ERR_INVALID_STATE || consecutive_failures >= 2) {
                ESP_LOGW(TAG_WiFi, "requesting audio ws destroy due to repeated failures");
                request_audio_ws_destroy();

                // Try a single requeue to give manager time to recreate the client
                if (requeue_count < MAX_REQUEUE_PER_FRAME) {
                    requeue_count++;
                    if (xQueueSend(wifi_send_q, &frame, pdMS_TO_TICKS(200)) == pdTRUE) {
                        ESP_LOGI(TAG_WiFi, "requeued frame after request_destroy (try=%d)", requeue_count);
                        frame = NULL; // ownership transferred
                        // Wait a short while for manager to do its job before continuing
                        vTaskDelay(pdMS_TO_TICKS(POST_DESTROY_WAIT_MS));
                        break; // break chunk loop and wait for reconnect
                    } else {
                        ESP_LOGW(TAG_WiFi, "requeue failed; will drop frame");
                        free_wifi_frame(frame);
                        frame = NULL;
                        break;
                    }
                } else {
                    ESP_LOGW(TAG_WiFi, "requeue limit reached; dropping frame");
                    free_wifi_frame(frame);
                    frame = NULL;
                    break;
                }
            } else {
                // Best-effort: skip this chunk (avoid blocking forever)
                ESP_LOGW(TAG_WiFi, "skipping problematic chunk and continuing (best-effort)");
                offset += take;
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }

        } // per-frame chunk loop

        if (frame) {
            free_wifi_frame(frame);
            frame = NULL;
        }

    } // forever
}


/* ---------- I2S capture task (non-blocking enqueue, WDT-friendly) ---------- */
void i2s_capture_task(void *pvParameters)
{
    ESP_LOGI("I2S_CAP", "i2s_capture_task start (low-latency enqueue)");
    esp_task_wdt_add(NULL);

    const size_t bytes_per_read = I2S_BUF_SIZE;
    const size_t target_us = 120000;  // best-effort cadence target

    static uint8_t batch_buf[MAX_BATCH_PCM_BYTES];
    size_t batch_offset = 0;
    size_t batch_frames = 0;
    uint64_t last_ts = esp_timer_get_time();

    frames_dropped = 0;

    for (;;) {
        /* Wait until mic_active (notify-based) */
        while (!atomic_load(&mic_active)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            if (!atomic_load(&mic_active)) {
                esp_task_wdt_reset();
                continue;
            }
            break;
        }

        /* Ensure rx_chan exists; try to init once if missing */
        if (!rx_chan && atomic_load(&mic_active)) {
            ESP_LOGI("I2S_CAP", "rx_chan NULL — initializing");
            i2s_init_inmp441();
            if (!rx_chan) {
                ESP_LOGE("I2S_CAP", "i2s init failed; retrying in 200 ms");
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        batch_offset = 0;
        batch_frames = 0;
        last_ts = esp_timer_get_time();

        /* Main capture loop — blocking read but non-blocking enqueue */
        while (atomic_load(&mic_active)) {
            esp_task_wdt_reset();

            size_t bytes_read = 0;

            if (!rx_chan) {
                ESP_LOGW("I2S_CAP", "rx_chan unexpectedly NULL inside capture");
                break;
            }

            esp_err_t ret = i2s_channel_read(rx_chan,
                                            (void *)i2s_dma_buf,
                                            bytes_per_read,
                                            &bytes_read,
                                            portMAX_DELAY);
            if (ret == ESP_OK && bytes_read == 0) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }

            if (ret != ESP_OK) {
                ESP_LOGW("I2S_CAP", "i2s_channel_read returned %s — attempting graceful recovery", esp_err_to_name(ret));
                if (rx_chan) {
                    i2s_channel_disable(rx_chan);
                    i2s_del_channel(rx_chan);
                    rx_chan = NULL;
                }
                break;
            }

            /* Convert 32-bit samples -> 16-bit and append to batch buffer */
            int32_t *src = (int32_t *)i2s_dma_buf;
            size_t samples = bytes_read / sizeof(int32_t);
            size_t max_samples = (MAX_BATCH_PCM_BYTES - batch_offset) / sizeof(int16_t);
            if (samples > max_samples) samples = max_samples;

            int16_t *dst = (int16_t *)(batch_buf + batch_offset);
            for (size_t i = 0; i < samples; ++i) {
                dst[i] = (int16_t)(src[i] >> 8);
            }

            ESP_LOGI("I2S_DBG", "BCK=%d WS=%d DIN=%d",
                gpio_get_level(2),
                gpio_get_level(3),
                gpio_get_level(5));

            int32_t sample;
            size_t br = 0;
            for (int i = 0; i < 10; i++) {
                i2s_channel_read(rx_chan, &sample, sizeof(sample), &br, portMAX_DELAY);
                ESP_LOGI("I2S_SAMPLE", "raw sample: %ld (br=%d)", sample, br);
            }


            batch_offset += samples * sizeof(int16_t);
            batch_frames++;

            /* When enough frames collected, package and try to enqueue (non-blocking) */
            if (batch_frames >= FRAMES_PER_BATCH) {
                uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
                uint16_t seq = (uint16_t)__atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
                size_t frame_len = 8 + batch_offset;

                pack_header(audio_frame_buf, 0x01, AUDIO_FLAG_PCM, seq, ts);

                wifi_frame_t *wframe = alloc_wifi_frame(frame_len);
                if (wframe) {
                    memcpy(wframe->data, audio_frame_buf, 8);            // header
                    memcpy(wframe->data + 8, batch_buf, batch_offset);  // payload
                    wframe->len = frame_len;

                    // Non-blocking enqueue: immediate drop if queue full (UDP-like)
                    if (xQueueSend(wifi_send_q, &wframe, 0) != pdTRUE) {
                        // queue full -> drop frame, free buffers
                        free_wifi_frame(wframe);
                        frames_dropped++;
                        ESP_LOGW("WiFiQueue", "Queue full — dropped batch (frames_dropped=%u)", frames_dropped);
                    } else {
                        frames_sent++;
                    }
                } else {
                    ESP_LOGE("WiFiQueue", "alloc_wifi_frame failed — dropping batch");
                    frames_dropped++;
                }

                batch_offset = 0;
                batch_frames = 0;

                // keep rough cadence but let reads drive pace; small breathe for scheduler
                uint64_t now_ts = esp_timer_get_time();
                int64_t remain_us = (int64_t)target_us - (int64_t)(now_ts - last_ts);
                if (remain_us > 0) {
                    vTaskDelay(pdMS_TO_TICKS((remain_us + 999) / 1000));
                } else {
                    // if we're late, yield a tiny bit to let sender catch up
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                last_ts = esp_timer_get_time();
            }
        } /* inner while (mic_active) */

        /* Graceful disable on mic stop */
        ESP_LOGI("I2S_CAP", "Mic deactivated — disabling RX channel (if present)");
        if (rx_chan) {
            esp_err_t dis = i2s_channel_disable(rx_chan);
            if (dis != ESP_OK && dis != ESP_ERR_INVALID_STATE) {
                ESP_LOGW("I2S_CAP", "i2s_channel_disable rc=%s", esp_err_to_name(dis));
            }
        }

        if (atomic_load(&i2s_deinit_requested)) {
            atomic_store(&i2s_deinit_requested, false);
            i2s_del_channel_from_capture();
        }

        esp_task_wdt_reset();
    } /* outer for */
}


static TaskHandle_t wifi_watchdog_handle = NULL;

void wifi_watchdog_task(void *arg)
{
    ESP_LOGI("WiFiWDT", "wifi_watchdog_task started");
    atomic_store(&wifi_wdt_active, true);

    const uint32_t reconnect_interval_ms = 10000;
    // how long we require stable connection before auto-stopping the watchdog (optional)
    const uint32_t stable_ms_required = 10000;
    uint64_t stable_since = 0;

    while (atomic_load(&wifi_wdt_active)) {
        // If the wifi stack isn't up yet, wait a bit and keep looping
        if (!wifi_stack_started) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        bool is_connected = atomic_load(&wifi_connected);

        if (!is_connected) {
            ESP_LOGW("WiFiWDT", "Not connected — requesting esp_wifi_connect()");
            esp_err_t rc = esp_wifi_connect();
            if (rc != ESP_OK) {
                ESP_LOGW("WiFiWDT", "esp_wifi_connect returned %s", esp_err_to_name(rc));
            }
            stable_since = 0; // reset stable timer
        } else {
            // connection present: start/continue stable timer
            if (stable_since == 0) stable_since = esp_timer_get_time() / 1000ULL;
            else {
                uint64_t now = esp_timer_get_time() / 1000ULL;
                if (now - stable_since >= stable_ms_required) {
                    ESP_LOGI("WiFiWDT", "Wi-Fi stable for %u ms — stopping watchdog", stable_ms_required);
                    atomic_store(&wifi_wdt_active, false);
                    break;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(reconnect_interval_ms));
    }

    ESP_LOGI("WiFiWDT", "wifi_watchdog_task exiting");
    wifi_watchdog_handle = NULL;
    vTaskDelete(NULL);
}

static void start_wifi_watchdog_if_needed(void)
{
    if (wifi_watchdog_handle != NULL) return; // already running
    if (xTaskCreate(wifi_watchdog_task, "wifi_watchdog", 4096, NULL, 5, &wifi_watchdog_handle) != pdPASS) {
        ESP_LOGE("WiFiWDT", "Failed to start wifi_watchdog_task");
        wifi_watchdog_handle = NULL;
    } else {
        ESP_LOGI("WiFiWDT", "Watchdog started");
    }
}

static void stop_wifi_watchdog(void)
{
    // polite stop: set flag and wait for task to exit
    atomic_store(&wifi_wdt_active, false);
    if (wifi_watchdog_handle != NULL) {
        // don't call vTaskDelete() blindly from arbitrary context; the task will exit itself.
        // Optionally, wait a short time for it to exit:
        for (int i = 0; i < 10 && wifi_watchdog_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (wifi_watchdog_handle != NULL) {
            // as last resort, delete (only if you truly need immediate stop)
            vTaskDelete(wifi_watchdog_handle);
            wifi_watchdog_handle = NULL;
            ESP_LOGW("WiFiWDT", "Watchdog force-deleted");
        } else {
            ESP_LOGI("WiFiWDT", "Watchdog stopped cleanly");
        }
    }
}


/* ---------- FSR ADC (BLE Notify) ---------- */
void pSensor_task(void *arg)
{
    // Register this task with the Task WDT so esp_task_wdt_reset() calls are legal.
    // Use a short/normal WDT timeout configured elsewhere — this just registers the task.
    esp_err_t rc = esp_task_wdt_add(NULL);
    if (rc != ESP_OK) {
        ESP_LOGW("WiFiWDT", "pSensor: esp_task_wdt_add failed: %s", esp_err_to_name(rc));
    } else {
        ESP_LOGI("WiFiWDT", "pSensor: registered with Task WDT");
    }

    static adc_digi_output_data_t result[ADC_READ_LEN / sizeof(adc_digi_output_data_t)];
    uint32_t evt_size = 0;
    ESP_LOGI("ADC_CONT", "FSR pSensor_task running (event-driven)");

    while (1) {
        // reset WDT at loop start so long waits don't trigger
        esp_task_wdt_reset();

        if (!adc_running || !pSensor_enabled) {
            // If idle, still periodically reset the WDT
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (xQueueReceive(adc_evt_queue, &evt_size, pdMS_TO_TICKS(fsr_interval_ms))) {
            // another safety reset before handling data (BLE/HTTP can take time)
            esp_task_wdt_reset();

            uint32_t bytes_read = 0;
            esp_err_t ret = adc_continuous_read(adc_handle, (uint8_t *)result, sizeof(result), &bytes_read, 0);
            if (ret != ESP_OK || bytes_read < sizeof(adc_digi_output_data_t)) {
                // yield and reset WDT, don't spin
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }

            // process entries...
            // BEFORE any BLE notify or HTTP client call, reset watchdog again
            esp_task_wdt_reset();

            // ... your existing loop logic that builds payload and calls send_chunked_notify()
            // send_chunked_notify(...) will also call esp_task_wdt_reset() internally as intended.
            // But still call esp_task_wdt_reset() before/after heavy ops here too.

            esp_task_wdt_reset(); // after handling
        } else {
            // timed out waiting for ADC event — reset and continue
            esp_task_wdt_reset();
        }
    }

    // Clean up: unregister from WDT before exiting (won't be reached in your infinite task but good hygiene)
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

/* ---- Deferred JSON parsing task ---- */
static void control_task(void *arg)
{
    control_msg_t msg;
    while (1) {
        if (xQueueReceive(ctrl_queue, &msg, portMAX_DELAY) == pdTRUE) {
            LOG_CTRL("rx JSON: %s", msg.json);
            char ack_buf[160];
            snprintf(ack_buf, sizeof(ack_buf),
                     "{\"status\":\"RCVD\",\"msg\":\"%s\"}", msg.json ? msg.json : "");

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
static int __attribute__((unused)) send_chunked_notify(uint16_t conn_handle,
                               uint16_t attr_handle,
                               const uint8_t *frame,
                               size_t frame_len)
{
    if (!atomic_load(&bt_conn) || attr_handle == 0)
        return -1;

    if (!atomic_load(&mic_streaming_active)) {
        return -1;  // stop chunking instantly (caller treats as failure)
    }


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
        // Always copy the 8-byte header into the start of notify_buf on first chunk,
        // then copy payload chunks from frame+8+offset (skip the 8-byte header).
        if (offset == 0) {
            memcpy(notify_buf, frame, 8);
        }

        size_t safe_take = MIN(take, (size_t)(MAX_NOTIFY_BUF - 8));
        /* Source must skip the 8-byte header: payload starts at frame + 8.
           For chunk N we copy from frame + 8 + offset. */
        memcpy(notify_buf + 8, frame + 8 + offset, safe_take);

        // create mbuf only for the amount actually copied (8 + safe_take)
        struct os_mbuf *om = ble_hs_mbuf_from_flat(notify_buf, 8 + safe_take);

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

        /* advance by the number of bytes actually copied */
        offset += safe_take;
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

                int phy_rc = ble_gap_set_prefered_le_phy(g_conn_handle, BLE_HCI_LE_PHY_1M, BLE_HCI_LE_PHY_1M, 0);

                ESP_LOGI(TAG, "ble_gap_set_prefered_le_phy rc=%d", phy_rc);
                atomic_store(&bt_conn, true);

            } else {
                ESP_LOGW(TAG, "Connect failed; status=%d", event->connect.status);
                // Restart advertising
                ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
            }
            break;

            case BLE_GAP_EVENT_DISCONNECT:
                ESP_LOGW(TAG, "Client disconnected (reason=%d)", event->disconnect.reason);

                ESP_LOGI("BLE", "Client disconnected, stopping Wi-Fi watchdog...");
                if (wifi_watchdog_handle != NULL) {
                    stop_wifi_watchdog();
                    wifi_watchdog_handle = NULL;
                }


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

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t) handler_args;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            if (client == ws_client_ctrl) {
                atomic_store(&wifi_connected, true);
                atomic_store(&ws_ctrl_connected, true);
                ESP_LOGI("WS", "Control WS connected (%p)", client);
            } else if (client == ws_client_audio) {
                // give transport a moment to settle
                vTaskDelay(pdMS_TO_TICKS(50));
                atomic_store(&ws_audio_connected, true);
                ESP_LOGI("WS", "Audio WS connected (%p)", client);
            } else {
                ESP_LOGI("WS", "Unknown WS connected (%p)", client);
            }
            dump_ws_state("connected");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            if (client == ws_client_ctrl) {
                atomic_store(&wifi_connected, false);
                atomic_store(&ws_ctrl_connected, false);
                ESP_LOGW("WS", "Control WS disconnected");
                ws_action_t a = WS_ACTION_DESTROY_CTRL;
                xQueueSend(ws_mgr_q, &a, 0);
            } else if (client == ws_client_audio) {
                atomic_store(&ws_audio_connected, false);
                ESP_LOGW("WS", "Audio WS disconnected");
                if (ws_mgr_q) {
                    ws_action_t a = WS_ACTION_DESTROY_AUDIO;
                    if (xQueueSend(ws_mgr_q, &a, 0) != pdTRUE) {
                        ESP_LOGW("WS", "ws_mgr_q full, will attempt destroy later");
                    }
                } else {
                    ESP_LOGW("WS", "ws_mgr_q not ready — audio client left for cleanup");
                }
            } else {
                ESP_LOGW("WS", "Unknown WS disconnected");
            }
            dump_ws_state("disconnected");
            break;

        case WEBSOCKET_EVENT_DATA:
            if (!data) break;
            if (data->op_code == 0x9) {
                ESP_LOGI("WS", "PING from server");
                break;
            } else if (data->op_code == 0xA) {
                ESP_LOGI("WS", "PONG from server");
                break;
            }

            if (client == ws_client_ctrl) {
                const char *buf = data->data_ptr;
                size_t len = data->data_len;
                if (!buf || len < 2) break;
                bool printable = true;
                for (size_t i = 0; i < len; i++) {
                    if ((unsigned char)buf[i] < 0x09 || (unsigned char)buf[i] > 0x7E) {
                        printable = false;
                        break;
                    }
                }
                if (!printable) break;

                // Skip leading whitespace
                while (len && isspace((unsigned char)*buf)) { buf++; len--; }

                if (len < 2) break;
                if (!((*buf == '{' && buf[len - 1] == '}') || (*buf == '[' && buf[len - 1] == ']'))) break;

                cJSON *root = cJSON_ParseWithLength(buf, len);
                if (!root) break;
                cJSON *cmd = cJSON_GetObjectItem(root, "command");
                if (cmd && cJSON_IsObject(cmd)) {
                    char *json_str = cJSON_PrintUnformatted(cmd);
                    if (json_str) {
                        control_msg_t msg = { .len = strlen(json_str), .json = json_str };
                        if (!ctrl_queue || xQueueSend(ctrl_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
                            free(json_str);
                        }
                    }
                }
                cJSON_Delete(root);
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
        // explicit connect on start is okay; event-driven connect is safe
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        atomic_store(&wifi_connected, false);
        ESP_LOGW(WIFI_TAG, "Disconnected (reason=%d), retrying...", disc ? disc->reason : -1);
        
        if (disc) {
            switch (disc->reason) {
                case WIFI_REASON_AUTH_EXPIRE:
                case WIFI_REASON_AUTH_FAIL:
                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                    ESP_LOGE(WIFI_TAG, "❌ Authentication failed — wrong password or mismatched security mode.");
                    break;

                case WIFI_REASON_NO_AP_FOUND:
                    ESP_LOGE(WIFI_TAG, "🚫 No AP found for SSID '%s' — check name and 2.4GHz availability.", disc ? (char*)disc->ssid : "(unknown)");
                    break;

                case WIFI_REASON_ASSOC_FAIL:
                case WIFI_REASON_ASSOC_LEAVE:
                    ESP_LOGE(WIFI_TAG, "⚠️ Association failed — signal too weak or AP refused connection.");
                    break;

                case WIFI_REASON_BEACON_TIMEOUT:
                    ESP_LOGE(WIFI_TAG, "📡 Beacon timeout — lost AP signal (weak link or interference).");
                    break;

                case WIFI_REASON_HANDSHAKE_TIMEOUT:
                    ESP_LOGE(WIFI_TAG, "⚠️ WPA handshake timeout — router didn't respond in time.");
                    break;

                case WIFI_REASON_MIC_FAILURE:
                    ESP_LOGE(WIFI_TAG, "🔒 WPA MIC failure — corrupted frame or bad encryption key.");
                    break;

                case WIFI_REASON_AUTH_LEAVE:
                    ESP_LOGE(WIFI_TAG, "🔄 Deauthenticated by AP — router booted you.");
                    break;

                default:
                    ESP_LOGW(WIFI_TAG, "🤔 Unknown Wi-Fi disconnect reason: %d", disc->reason);
                    break;
            }
        } else {
            ESP_LOGW(WIFI_TAG, "Wi-Fi disconnect: event_data was null");
        }

        if (wifi_http_client) {
            esp_http_client_cleanup(wifi_http_client);
            wifi_http_client = NULL;
        }
        // Let the watchdog manage backoff/reconnect attempts; still call connect once
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

        ESP_LOGI(WIFI_TAG, "Flushing stale WS handles before reconnect...");
        if (ws_client_ctrl) {
            esp_websocket_client_stop(ws_client_ctrl);
            esp_websocket_client_destroy(ws_client_ctrl);
            ws_client_ctrl = NULL;
        }
        if (ws_client_audio) {
            esp_websocket_client_stop(ws_client_audio);
            esp_websocket_client_destroy(ws_client_audio);
            ws_client_audio = NULL;
        }

        ESP_LOGI(WIFI_TAG, "Connected to Wi-Fi");

        // Do NOT use SSL transport here unless URI starts with wss://
        // The esp-tls layer will hang like a drunken cat on a ceiling fan.

        // Create / start control WS client
        esp_websocket_client_config_t ctrl_cfg = {
            .uri = WS_CTRL_URI,
            .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
            .reconnect_timeout_ms = 3000,
            .keep_alive_idle = 10,
            .keep_alive_interval = 10,
            .keep_alive_count = 3,
            .network_timeout_ms = 10000
        };

        if (ws_client_ctrl) {
            esp_websocket_client_stop(ws_client_ctrl);
            esp_websocket_client_destroy(ws_client_ctrl);
            ws_client_ctrl = NULL;
        }

        ws_client_ctrl = esp_websocket_client_init(&ctrl_cfg);
        if (ws_client_ctrl) {
            esp_websocket_register_events(ws_client_ctrl, WEBSOCKET_EVENT_ANY,
                                          websocket_event_handler, (void *)ws_client_ctrl);
            esp_websocket_client_start(ws_client_ctrl);
            ESP_LOGI(WIFI_TAG, "Control WS client started (%s)", WS_CTRL_URI);
            atomic_store(&ws_ctrl_connected, true);
        } else {
            ESP_LOGW(WIFI_TAG, "Failed to init control WS client");
            atomic_store(&ws_ctrl_connected, false);
        }

        esp_websocket_client_config_t audio_cfg = {
            .uri = WS_AUDIO_URI,
            .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
            .reconnect_timeout_ms = 5000,
            .network_timeout_ms = 12000,
            .task_stack = 20 * 1024,
            .task_prio  = 6,
            .buffer_size = 4096,
            .ping_interval_sec = 30,
            .disable_auto_reconnect = false,
            .disable_pingpong_discon = true,   // <-- THE RIGHT FIELD
        };

        if (ws_client_audio) {
            esp_websocket_client_stop(ws_client_audio);
            esp_websocket_client_destroy(ws_client_audio);
            ws_client_audio = NULL;
        }

        ws_client_audio = esp_websocket_client_init(&audio_cfg);
        if (ws_client_audio) {
            esp_websocket_register_events(ws_client_audio, WEBSOCKET_EVENT_ANY,
                                          websocket_event_handler, (void *)ws_client_audio);
            esp_websocket_client_start(ws_client_audio);
            ESP_LOGI(WIFI_TAG, "Audio WS client started (%s)", WS_AUDIO_URI);
            atomic_store(&ws_audio_connected, true);
        } else {
            ESP_LOGW(WIFI_TAG, "Failed to init audio WS client");
            atomic_store(&ws_audio_connected, false);
        }

        ESP_LOGI(WIFI_TAG, "Starting HTTP server");
        if (!stream_server) {
            start_http_server();
        }
    }
}

// ─────────────────────────────────────────────
// BLE callback to receive Wi-Fi credentials
// ─────────────────────────────────────────────
void ble_on_wifi_creds_received(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW("BLE_CFG", "Invalid JSON in BLE payload");
        return;
    }

    const cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItem(root, "pass");
    const cJSON *j_ip   = cJSON_GetObjectItem(root, "host_ip");
    const cJSON *j_port = cJSON_GetObjectItem(root, "host_port");

    if (!cJSON_IsString(j_ssid) || !cJSON_IsString(j_pass) || !cJSON_IsString(j_ip) || !cJSON_IsString(j_port)) {
        ESP_LOGE("BLE_CFG", "Invalid BLE JSON received");
        cJSON_Delete(root);
        ble_send_json_response("{\"status\":\"ERR\",\"msg\":\"Invalid JSON\"}");
        return;
    }

    wifi_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    strncpy(creds.ssid, j_ssid->valuestring, sizeof(creds.ssid)-1);
    strncpy(creds.pass, j_pass->valuestring, sizeof(creds.pass)-1);
    strncpy(creds.ip,   j_ip->valuestring,   sizeof(creds.ip)-1);
    strncpy(creds.port, j_port->valuestring, sizeof(creds.port)-1);

    // Save to NVS immediately (safe from BLE context)
    // save_wifi_creds(creds.ssid, creds.pass, creds.ip, creds.port);

    /* ESP_LOGI("BLE_CFG", "Parsed Wi-Fi creds: ssid=%s pass=%s", creds.ssid, creds.pass);
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_close(nvs);
    } */

    // 🔧 Immediately update runtime URIs
    snprintf(WS_CTRL_URI, sizeof(WS_CTRL_URI), "ws://%s:%s/ws", creds.ip, creds.port);
    snprintf(WS_AUDIO_URI, sizeof(WS_AUDIO_URI), "ws://%s:%s/data/audio", creds.ip, creds.port);
    snprintf(OTA_URL, sizeof(OTA_URL), "http://%s:%s/ota/latest.bin", creds.ip, creds.port);
    snprintf(TELEMETRY_URL, sizeof(TELEMETRY_URL), "http://%s:%s/esp/telemetry", creds.ip, creds.port);

    ESP_LOGI("BLE_CFG", "Updated runtime URIs:");
    ESP_LOGI("BLE_CFG", "  CTRL=%s", WS_CTRL_URI);
    ESP_LOGI("BLE_CFG", "  AUDIO=%s", WS_AUDIO_URI);

    // ACK right away
    ble_send_json_response("{\"status\":\"OK\",\"msg\":\"WiFi credentials saved\"}");


    // If Wi-Fi stack already started, update config in short task to avoid double-init.
    wifi_creds_t *heap_creds = malloc(sizeof(wifi_creds_t));
    if (!heap_creds) {
        ESP_LOGE("BLE_CFG", "malloc failed for wifi creds");
        cJSON_Delete(root);
        return;
    }
    memcpy(heap_creds, &creds, sizeof(wifi_creds_t));

    if (wifi_stack_started) {

        if (ws_client_ctrl) {
            esp_websocket_client_stop(ws_client_ctrl);
            esp_websocket_client_destroy(ws_client_ctrl);
            ws_client_ctrl = NULL;
        }
        if (ws_client_audio) {
            esp_websocket_client_stop(ws_client_audio);
            esp_websocket_client_destroy(ws_client_audio);
            ws_client_audio = NULL;
        }

        start_wifi_watchdog_if_needed();

        // Wi-Fi driver already initialized — reconfigure instead of reinitializing
        BaseType_t rc = xTaskCreate(wifi_reconfigure_task, "wifi_recfg", 4096, heap_creds, 5, NULL);
        if (rc != pdPASS) {
            ESP_LOGE("BLE_CFG", "Failed to create wifi_reconfigure_task");
            free(heap_creds);
        } else {
            ESP_LOGI("BLE_CFG", "wifi_reconfigure_task created (reconfigure path)");
        }
    } else {
        // Wi-Fi stack not started — do full init in wifi_init_task
        BaseType_t rc = xTaskCreate(wifi_init_task, "wifi_init_task", 12288, heap_creds, 3, NULL);
        ESP_LOGI("BLE_CFG", "wifi_init_task create result: %d", rc);
        if (rc != pdPASS) {
            ESP_LOGE("BLE_CFG", "Failed to create wifi_init_task");
            free(heap_creds);
        } else {
            ESP_LOGI("BLE_CFG", "wifi_init_task created successfully");
        }
    }


    cJSON_Delete(root);
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
        ESP_LOGI(TAG_OTA, "OTA complete, shutting down safely in 2s...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_stop();
        esp_bt_controller_disable();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        ESP_LOGE(TAG_OTA, "OTA failed: %s", esp_err_to_name(ret));
    }

    vTaskDelete(NULL); 
}

#if ENABLE_TELEMETRY
static void telemetry_push_task(void *arg) {

    if (!ENABLE_TELEMETRY) { // defensive: compile-time true branch only
        ESP_LOGW("TELEMETRY", "telemetry disabled (compile-time) — exiting task");
        vTaskDelete(NULL);
    }

    while (1) {
        if (!atomic_load(&wifi_connected) || !TELEMETRY_URL[0]) {
            // If not connected or URL not set, wait and re-check.
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        float temp = read_temperature_c();
        if (temp < -500.0f) temp = 0.0f; // guard if sensor unavailable

        char json[256];
        snprintf(json, sizeof(json),
                "{\"device_id\":\"speechster_b1_s3\",\"heap\":%u,\"frames_sent\":%" PRIu32 ",\"temp\":%.2f}",
                (unsigned)esp_get_free_heap_size(), frames_sent, temp);

        esp_http_client_config_t cfg = {
            .url = TELEMETRY_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 3000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, json, strlen(json));
            esp_err_t rc = esp_http_client_perform(client);
            if (rc != ESP_OK) {
                ESP_LOGW("TELEMETRY", "POST failed: %s", esp_err_to_name(rc));
            }
            esp_http_client_cleanup(client);
        } else {
            ESP_LOGW("TELEMETRY", "Failed to init HTTP client (skipping telemetry)");
        }

        vTaskDelay(pdMS_TO_TICKS(30000)); // every 30s
    }
}
#else
/* If telemetry is disabled at compile time, provide a tiny stub so references link cleanly. */
static void __attribute__((unused)) telemetry_push_task(void *arg) {
    ESP_LOGW("TELEMETRY", "telemetry compile-time disabled — task not running");
    vTaskDelete(NULL);
}
#endif

/* init on-chip temperature sensor (ESP32-S3) */
static void init_temp_sensor(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
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

/* Small smoothing filter to reduce temperature jitter */
static float smooth_temp(float new_temp)
{
    static float avg = -10000.0f;
    if (avg < -9000.0f) avg = new_temp;
    avg = (avg * 0.8f) + (new_temp * 0.2f);
    return avg;
}


/* Thermal watchdog — auto-throttle radios and stop audio if needed */
static void thermal_watchdog_task(void *arg)
{
    while (1) {
        float t = read_temperature_c();
        t = smooth_temp(t);
        if (t > -100.0f) { // valid reading

            if (!wifi_stack_started) {
                ESP_LOGD("THERMAL", "Wi-Fi stack not started yet; skipping TX power adjust");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }

            esp_err_t rc = ESP_OK;

            if (t >= THERMAL_CRIT_C) {
                ESP_LOGE("THERMAL", "CRITICAL TEMP %.2f °C — stopping mic & throttling radios", t);
                stop_mic_streaming();
                rc = esp_wifi_set_max_tx_power(8);   // ~2 dBm
            } else if (t >= THERMAL_WARN_C) {
                ESP_LOGW("THERMAL", "High temp %.2f °C — throttling radios", t);
                rc = esp_wifi_set_max_tx_power(32);  // ~8 dBm
            } else {
                rc = esp_wifi_set_max_tx_power(40);  // ~10 dBm (nominal)
            }

            if (rc != ESP_OK)
                ESP_LOGW("THERMAL", "esp_wifi_set_max_tx_power rc=%s", esp_err_to_name(rc));
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

                    /* 4️⃣ Then Wi-Fi to backend for logging (guarded) */
                    #if ENABLE_TELEMETRY
                        if (atomic_load(&wifi_connected) && TELEMETRY_URL[0]) {
                            esp_http_client_config_t cfg = {
                                .url = TELEMETRY_URL,
                                .method = HTTP_METHOD_POST,
                                .timeout_ms = 2000,
                            };
                            esp_http_client_handle_t c = esp_http_client_init(&cfg);
                            if (c) {
                                esp_http_client_set_header(c, "Content-Type", "application/json");
                                esp_http_client_set_post_field(c, die_msg, strlen(die_msg));
                                esp_err_t rc = esp_http_client_perform(c);
                                if (rc != ESP_OK) {
                                    ESP_LOGW("DIE_TEMP", "Emergency POST failed: %s", esp_err_to_name(rc));
                                }
                                esp_http_client_cleanup(c);
                            } else {
                                ESP_LOGW("DIE_TEMP", "Failed to init HTTP client for emergency telemetry");
                            }
                        } else {
                            ESP_LOGI("DIE_TEMP", "Telemetry disabled or TELEMETRY_URL empty — skipping emergency POST");
                        }
                    #else
                        ESP_LOGI("DIE_TEMP", "Telemetry compile-time disabled — skipping emergency POST");
                    #endif


                    // 5️⃣ Disable everything except radios

                    if (wifi_stack_started) {
                        esp_err_t rc = esp_wifi_stop();
                        ESP_LOGI("DIE_TEMP", "esp_wifi_stop rc=%s", esp_err_to_name(rc));
                        wifi_stack_started = false;
                    } else {
                        ESP_LOGW("DIE_TEMP", "Wi-Fi already stopped, skipping");
                    }
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
    // On S3 the esp_pm_config_t struct is available; falling back to esp_pm_config_t works on many IDF versions.
    // We'll use esp_pm_config_t as in your existing code but set light sleep enabled.
    esp_pm_config_t pmcfg = {
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

static void print_b1_banner() {

        // Hide cursor for cinematic effect
    printf("\033[?25l");

    // Cyan → White → Magenta gradient using precomputed ANSI 24-bit colors
    printf("\n");
    printf("\033[38;2;0;200;255m═══════════════════════════════════════════════════════════════\033[0m\n");
    
    vTaskDelay(pdMS_TO_TICKS(50));

    printf("\033[38;2;0;200;255m║\033[0m");
    vTaskDelay(pdMS_TO_TICKS(50));
    printf("                        \033[38;2;255;255;255mSPEECHSTER-B1\033[0m");
    vTaskDelay(pdMS_TO_TICKS(50));
    printf("                       \033[38;2;255;100;200m║\033[0m\n");
    vTaskDelay(pdMS_TO_TICKS(50));

    printf("\033[38;2;0;200;255m║\033[0m");
    vTaskDelay(pdMS_TO_TICKS(50));

    printf("            \033[38;2;255;150;230m“Where overheating meets overengineering.”\033[0m");
    vTaskDelay(pdMS_TO_TICKS(50));

    printf("       \033[38;2;255;100;200m║\033[0m\n");
    vTaskDelay(pdMS_TO_TICKS(50));


    printf("\033[38;2;255;120;255m═══════════════════════════════════════════════════════════════\033[0m\n\n");
    vTaskDelay(pdMS_TO_TICKS(50));

    // Restore cursor visibility
    printf("\033[?25h");
}

static void ws_manager_task(void *arg)
{
    ws_action_t act;
    while (1) {
        if (xQueueReceive(ws_mgr_q, &act, portMAX_DELAY) == pdTRUE) {
            switch (act) {
                case WS_ACTION_STOP_CTRL:
                    if (ws_client_ctrl) {
                        esp_websocket_client_stop(ws_client_ctrl);
                    }
                    break;
                case WS_ACTION_DESTROY_CTRL:
                    if (ws_client_ctrl) {
                        // must stop then destroy
                        esp_websocket_client_stop(ws_client_ctrl);
                        vTaskDelay(pdMS_TO_TICKS(20)); // tiny breathing room
                        esp_websocket_client_destroy(ws_client_ctrl);
                        ws_client_ctrl = NULL;
                        atomic_store(&ws_ctrl_connected, false);
                    }
                    break;
                case WS_ACTION_STOP_AUDIO:
                    if (ws_client_audio) {
                        esp_websocket_client_stop(ws_client_audio);
                    }
                    break;
                case WS_ACTION_DESTROY_AUDIO:
                    if (ws_client_audio) {
                        esp_websocket_client_stop(ws_client_audio);
                        vTaskDelay(pdMS_TO_TICKS(20));
                        esp_websocket_client_destroy(ws_client_audio);
                        ws_client_audio = NULL;
                        atomic_store(&ws_audio_connected, false);
                    }
                    break;
                default:
                    break;
            }
        }
    }
}


/* ---------- app_main ---------- */
void app_main(void) {
    esp_log_set_vprintf(vprintf);
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_log_level_set("NimBLE", ESP_LOG_ERROR);
    esp_log_level_set("I2S_CAP", ESP_LOG_DEBUG);

    esp_err_t esp_brownout_disable(void);
    esp_brownout_disable();


    ESP_LOGI(TAG, "Speechster B1 Starting Up");

    // Mutexes and basic prealloc
    state_lock = xSemaphoreCreateMutex();
    ringbuf_mutex = xSemaphoreCreateMutex();
    if (!state_lock || !ringbuf_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return;
    }

    // early: create ws lock
    ws_audio_lock = xSemaphoreCreateMutex();
    if (!ws_audio_lock) {
        ESP_LOGE(TAG, "Failed to create ws_audio_lock");
        return;
    }


    // --- Preallocate DMA Buffers ---
    // Ensure DMA buffer is at least the I2S read size (I2S_BUF_SIZE)
    const size_t frame_sample_bytes = ((AUDIO_SAMPLE_RATE / 1000) * AUDIO_FRAME_MS) * sizeof(int32_t);
    const size_t required_bytes = MAX(frame_sample_bytes, (size_t)I2S_BUF_SIZE);

    i2s_dma_buf = heap_caps_malloc(required_bytes, MALLOC_CAP_DMA);
    if (!i2s_dma_buf) {
        ESP_LOGE(TAG, "Failed to alloc I2S DMA buffers (need %u bytes)", (unsigned)required_bytes);
        return;
    }

    ctrl_queue = xQueueCreate(CTRL_QUEUE_LEN, sizeof(control_msg_t));
    // pointer-based queue now (each item = pointer to wifi_frame_t)
    wifi_send_q = xQueueCreate(WIFI_SEND_QUEUE_LEN, sizeof(wifi_frame_t *));

    if (!ctrl_queue || !wifi_send_q) {
        ESP_LOGE(TAG, "Queue alloc failed");
        return;
    }

    // --- Init NVS early for Wi-Fi / BLE ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // --- Bring up BLE first ---
    init_nimble();

    // Create WS manager queue early so event callbacks can enqueue safely
    ws_mgr_q = xQueueCreate(8, sizeof(ws_action_t));
    if (!ws_mgr_q) {
        ESP_LOGE(TAG, "ws_mgr_q create failed");
    } else {
        xTaskCreate(ws_manager_task, "ws_mgr", 4096, NULL, 5, NULL);
    }

    // ─── Power / PM setup ──────────────────────────────────────
    setup_power_management();
    xTaskCreatePinnedToCore(dynamic_pm_task, "pm_adapt", 4096, NULL, 2, NULL, 1);

    // ─── Temperature sensor init ───────────────────────────────
    init_temp_sensor();

    // ─── Core assignments ──────────────────────────────────────

    // Core 0 → network / control / light sensors
    xTaskCreatePinnedToCore(control_task, "ctrl_task", 6144, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(wifi_send_task, "wifi_send", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(pSensor_task, "pSensor", 4096, NULL, 3, NULL, 0);

    // Core 1 → audio / power / thermal / telemetry
    
    // Capture and store the task handle so notify_i2s_start/stop work
    if (xTaskCreatePinnedToCore(i2s_capture_task, "i2s_cap", 10*1024, NULL, 5, &i2s_task_handle, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create i2s_capture_task");
        i2s_task_handle = NULL;
    }
    xTaskCreatePinnedToCore(thermal_watchdog_task, "thermal_wd", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(die_temperature_task, "die_temp", 4096, NULL, 5, NULL, 1);

    #if ENABLE_TELEMETRY
        if (xTaskCreatePinnedToCore(telemetry_push_task, "telemetry_push", 4096, NULL, 3, NULL, 1) != pdPASS) {
            ESP_LOGW(TAG, "Failed to create telemetry_push_task (will continue without telemetry)");
        } else {
            ESP_LOGI(TAG, "telemetry_push_task started");
        }
    #else
        ESP_LOGI(TAG, "telemetry disabled at compile time — telemetry task not created");
    #endif

/*    // --- Spawn mic test only after I²S fully ready ---
    vTaskDelay(pdMS_TO_TICKS(500));  // give time for I²S and DMA init
    if (rx_chan) {
        ESP_LOGI("DBG", "Spawning mic_test_task_debug (I²S ready)");
        xTaskCreate(mic_test_task_debug, "mic_debug", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGE("DBG", "Skipping mic debug — rx_chan not initialized");
    } */

    vTaskDelay(pdMS_TO_TICKS(300));
    printf("\033[2J\033[H");  // Clear screen
    print_b1_banner();
    
    ESP_LOGI(TAG, "%s Build %s (%s) — IDF %s",
             PROJECT_NAME, __DATE__, __TIME__, esp_get_idf_version());
}