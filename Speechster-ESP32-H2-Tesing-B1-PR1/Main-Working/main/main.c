// ─────────────────────────────────────────────
// Speechster-B1 BLE Audio Streamer — v0.9 UNSTABLE
// Oct-2025 build (ESP-IDF 5.5.1)
// Safe aggregation, adaptive BLE pacing, heap-based buffers
// ─────────────────────────────────────────────
// INMP441 I2S pins: SD=GPIO10, SCK=GPIO3, WS=GPIO14 (L/R tied to GND -> left channel)
// FSR402 ADC: ADC1 channel 1

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcpp"

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

/* --- Suppress deprecation warnings for legacy driver headers (keeps code compatible with current toolchain)
       This avoids the noisy compiler warnings you saw while retaining the well-tested APIs you already used.
       If you prefer a full migration to the new drivers (i2s_std, esp_adc/adc_oneshot), I can provide that separately. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "driver/i2s_std.h"
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

/* Include Cache Header (portable across cacheless MCUs like ESP32-H2) */
#if __has_include("esp_cache.h")
#include "esp_cache.h"
#else
// Some targets (e.g. ESP32-H2) have no cache API — define a no-op
#define esp_cache_writeback_all()  do {} while(0)
#endif

/* On chips where esp_cache_writeback_all() still isn't declared (like H2),
   silence the compiler by defining it as a weak inline no-op. */
#ifndef esp_cache_writeback_all
static inline void esp_cache_writeback_all(void) {}
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
static heap_trace_record_t trace_record[HEAP_TRACE_RECORDS];
static i2s_chan_handle_t rx_chan = NULL; 
static uint32_t rb_overflow_count = 0;
static uint32_t ble_delay_ms = 10;

#define I2S_PORT_NUM         I2S_NUM_0
#define I2S_BCK_IO           3
#define I2S_WS_IO            14
#define I2S_DATA_IN_IO       10
#define I2S_NUM         I2S_NUM_0
#define SAMPLE_BUF_LEN  320  // Number of samples per read


#define FSR_ADC_CHANNEL      ADC_CHANNEL_1
#define CONSERVATIVE_LL_OCTETS 220
static uint32_t fsr_interval_ms = 50; // default 50ms, can be changed via JSON

/* BLE custom UUIDs (browser should use the same strings when connecting) */
static const char *SERVICE_UUID_STR       = "0000feed-0000-1000-8000-00805f9b34fb";
static const char *CHAR_CONTROL_UUID_STR = "0000feed-0001-0000-8000-00805f9b34fb";
static const char *CHAR_STREAM_UUID_STR  = "0000feed-0002-0000-8000-00805f9b34fb";

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
static uint16_t calc_checksum16(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    // Fold to 16 bits and wrap carry
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}


/* Runtime State */
static const uint8_t AUDIO_FLAG_PCM = 0x00;
static volatile bool fsr_enabled = false;
static uint32_t seq_counter = 0;
static uint8_t *agg_buf = NULL;
static volatile uint64_t idle_ticks = 0;
static uint64_t last_total_ticks = 0;
static SemaphoreHandle_t ringbuf_mutex = NULL; // Protects ringbuffer access without disabling interrupts
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

static RingbufHandle_t audio_rb = NULL;
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
static int consecutive_ble_failures = 0;
static int64_t last_ble_fail_ts = 0;

// ======== Health Monitoring ========
static int64_t last_health_ts = 0;
static uint32_t frames_sent = 0;
static size_t total_bytes_sent = 0;

/* ---------- I2S (INMP441) ---------- */
static void i2s_init_inmp441(void) {
    i2s_chan_config_t chan_cfg = {
        .id = I2S_PORT_NUM,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 512,
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


void mic_test_task(void *param) {
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

// Global or static lock (safe to share between both funcs)
//static portMUX_TYPE ringbuf_mux = portMUX_INITIALIZER_UNLOCKED;

static void *ringbuf_receive_safe(RingbufHandle_t rb, size_t *item_size, TickType_t ticks_to_wait) {
    void *item = NULL;

    // take mutex (non-blocking fast path)
    if (xSemaphoreTake(ringbuf_mutex, pdMS_TO_TICKS(0)) == pdTRUE) {
        item = xRingbufferReceive(rb, item_size, 0); // try immediate
        xSemaphoreGive(ringbuf_mutex);

        if (item) return item;
    }

    // slow path: wait with mutex taken
    if (xSemaphoreTake(ringbuf_mutex, ticks_to_wait) == pdTRUE) {
        item = xRingbufferReceive(rb, item_size, 0);
        xSemaphoreGive(ringbuf_mutex);
    }
    return item;
}

static BaseType_t ringbuf_send_safe(RingbufHandle_t rb, const void *item, size_t size, TickType_t ticks_to_wait) {
    BaseType_t rc = pdFALSE;

    if (xSemaphoreTake(ringbuf_mutex, pdMS_TO_TICKS(0)) == pdTRUE) {
        rc = xRingbufferSend(rb, item, size, 0);
        xSemaphoreGive(ringbuf_mutex);
        if (rc == pdTRUE) return rc;
    }

    if (xSemaphoreTake(ringbuf_mutex, ticks_to_wait) == pdTRUE) {
        rc = xRingbufferSend(rb, item, size, ticks_to_wait);
        xSemaphoreGive(ringbuf_mutex);
    }
    return rc;
}

static void ringbuf_flush(RingbufHandle_t rb) {
    size_t sz;
    void *it;
    while ((it = ringbuf_receive_safe(rb, &sz, 0)) != NULL) {
        vRingbufferReturnItem(rb, it);
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

/* ---------- Heap-safe pre-allocated buffers ---------- */
#define MAX_FRAME_SIZE   (8 + AUDIO_READ_BYTES)  // 8-byte header + PCM
static uint8_t audio_frame_buf[MAX_FRAME_SIZE]; // pre-allocated buffer for PCM frames

/* ---------- I2S capture task (heap-safe) ---------- */
static void i2s_capture_task(void *arg)
{
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "i2s_capture_task start (real-time mode)");

    const uint32_t frame_ms = 10; // 10 ms per read (real-time tuned)
    const size_t frame_samples = (AUDIO_SAMPLE_RATE / 1000) * frame_ms; // 160 samples @16kHz
    const size_t bytes_per_read = frame_samples * sizeof(int32_t);
    const size_t pcm_bytes = frame_samples * sizeof(int16_t);
    const TickType_t i2s_read_timeout = pdMS_TO_TICKS(8);
    const TickType_t push_timeout = pdMS_TO_TICKS(5);

    int32_t *i2s_buf = (int32_t *)pvPortMalloc(bytes_per_read);
    int16_t *pcm_down = (int16_t *)pvPortMalloc(pcm_bytes);
    if (!i2s_buf || !pcm_down) {
        ESP_LOGE(TAG, "Failed to alloc I2S buffers");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        esp_task_wdt_reset();
        taskYIELD();

        if (!atomic_load(&mic_active)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_chan, i2s_buf, bytes_per_read, &bytes_read, i2s_read_timeout);
        esp_task_wdt_reset();

        if (ret != ESP_OK || bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        size_t samples = bytes_read / sizeof(int32_t);
        if (samples > frame_samples) samples = frame_samples;

        for (size_t i = 0; i < samples; i++)
            pcm_down[i] = (int16_t)(i2s_buf[i] >> 8);

        size_t frame_len = 8 + (samples * sizeof(int16_t));
        if (frame_len > MAX_FRAME_SIZE) frame_len = MAX_FRAME_SIZE;

        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint16_t seq = (uint16_t)__atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
        pack_header(audio_frame_buf, 0x01, AUDIO_FLAG_PCM, seq, ts);
        memcpy(audio_frame_buf + 8, pcm_down, frame_len - 8);

        if (ringbuf_send_safe(audio_rb, audio_frame_buf, frame_len, push_timeout) != pdTRUE) {
            rb_overflow_count++;
            ESP_LOGW(TAG, "audio_rb overflow (seq=%u)", seq);
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1)); // small breathing delay
    }

    vPortFree(i2s_buf);
    vPortFree(pcm_down);
    vTaskDelete(NULL);
}

/* ---------- FSR ADC polling (watchdog-safe) ---------- */
static void fsr_task(void *arg)
{
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "fsr_task start (ADC channel %d)", FSR_ADC_CHANNEL);

    static uint8_t fsr_frame[12];
    uint8_t payload[2];

    while (1) {
        esp_task_wdt_reset();

        if (!fsr_enabled) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Read ADC (feed WDT before/after)
        esp_task_wdt_reset();
        int raw = adc1_get_raw(FSR_ADC_CHANNEL);
        esp_task_wdt_reset();

        payload[0] = raw & 0xFF;
        payload[1] = (raw >> 8) & 0xFF;

        // Build frame
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
        pack_header(fsr_frame, 0x02, 0x00, 0, ts);
        memcpy(fsr_frame + 8, payload, sizeof(payload));

        // Try to send with a very short timeout; skip if full
        BaseType_t ok = xRingbufferSend(audio_rb, fsr_frame, sizeof(fsr_frame), pdMS_TO_TICKS(5));
        if (ok != pdTRUE) {
            ESP_LOGW(TAG, "FSR ringbuffer full — skipping frame");
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Feed WDT and yield
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(fsr_interval_ms));  // default 50ms
    }
}

/* ---- Deferred JSON parsing task ---- */
static void control_task(void *arg)
{
    control_msg_t msg;
    while (1) {
        if (xQueueReceive(ctrl_queue, &msg, portMAX_DELAY) == pdTRUE) {
            LOG_CTRL("rx JSON: %s", msg.json);

            cJSON *root = cJSON_Parse(msg.json);
            if (root) {
                xSemaphoreTake(state_lock, portMAX_DELAY);

                cJSON *jmic = cJSON_GetObjectItem(root, "mic");
                if (jmic && cJSON_IsBool(jmic)) {
                    if (cJSON_IsTrue(jmic)){
                        vTaskDelay(pdMS_TO_TICKS(300));
                        start_mic_streaming();
                        vTaskDelay(pdMS_TO_TICKS(10)); // let scheduler breathe before heavy I2S reads start
                    } else {
                        stop_mic_streaming();
                    }
                    LOG_JSON_PARSE("mic=%d", (int)atomic_load(&mic_active));
                }

                cJSON *jfsr = cJSON_GetObjectItem(root, "fsr");
                if (jfsr && cJSON_IsBool(jfsr)) {
                    fsr_enabled = cJSON_IsTrue(jfsr);
                    LOG_JSON_PARSE("fsr=%d", fsr_enabled);
                }

                cJSON *jdelay = cJSON_GetObjectItem(root, "ble_delay_ms");
                if (jdelay && cJSON_IsNumber(jdelay)) {
                    uint32_t v = (uint32_t) jdelay->valuedouble;
                    if (v >= 5 && v <= 200) {
                        ble_delay_ms = v;
                        LOG_JSON_PARSE("ble_delay_ms=%u", ble_delay_ms);
                    }
                }

                xSemaphoreGive(state_lock);
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
    chunk_size = MIN(chunk_size, 80);

    if (frame_len > STREAM_SLICE_MAX)
        frame_len = STREAM_SLICE_MAX;

    size_t offset = 0;
    int consecutive_failures = 0;

    while (offset < frame_len) {
        esp_task_wdt_reset();

        size_t remaining = frame_len - offset;
        size_t take = MIN(chunk_size, remaining);

        uint8_t buf[8 + 100];
        memcpy(buf, frame, 8);
        memcpy(buf + 8, frame + offset, take);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, 8 + take);
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

        // pacing: small delay between chunks
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    return 0;
}

static void ble_sender_task(void *arg)
{
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "ble_sender_task start");

    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        esp_task_wdt_reset();

        if (!atomic_load(&bt_conn) || g_stream_attr_handle == 0 || !atomic_load(&streaming)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t item_size = 0;
        uint8_t *frame = (uint8_t *)xRingbufferReceiveUpTo(audio_rb, &item_size, pdMS_TO_TICKS(10), 512);

        if (!frame) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        int rc = send_chunked_notify(g_conn_handle, g_stream_attr_handle, frame, item_size);
        vRingbufferReturnItem(audio_rb, frame);

        if (rc != 0) {
            ESP_LOGW(TAG, "Frame dropped after send failure (size=%zu)", item_size);
            vTaskDelay(pdMS_TO_TICKS(10));  // backoff before next frame
            continue;
        }

        // pacing between frames (~8 ms)
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(8));
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

                // Set preferred MTU
                int mtu_rc = ble_att_set_preferred_mtu(517);
                ESP_LOGI(TAG, "ble_att_set_preferred_mtu rc=%d", mtu_rc);

                // Request short connection interval: 7.5 ms
                struct ble_gap_upd_params params = {
                    .itvl_min = 6,   // 6 * 1.25 ms = 7.5 ms
                    .itvl_max = 6,
                    .latency  = 0,
                    .supervision_timeout = 400, // 400 * 10 ms = 4 s
                };
                int upd_rc = ble_gap_update_params(g_conn_handle, &params);
                ESP_LOGI(TAG, "ble_gap_update_params rc=%d", upd_rc);

                // Request 2 Mbps PHY (NimBLE v5.x)
                int phy_rc = ble_gap_set_prefered_le_phy(
                    g_conn_handle,
                    BLE_HCI_LE_PHY_2M_PREF_MASK,
                    BLE_HCI_LE_PHY_2M_PREF_MASK,
                    0
                );
                ESP_LOGI(TAG, "ble_gap_set_prefered_le_phy rc=%d", phy_rc);
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

                // Flush pending frames
                ringbuf_flush(audio_rb);
                ble_backoff_ms = 5;
                consecutive_ble_failures = 0;

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
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;
    nimble_port_freertos_init(nimble_host_task);
}

extern uint32_t frames_sent;
extern size_t total_bytes_sent;

static void telemetry_task(void *arg) {
    static uint32_t last_frames = 0;
    static size_t last_bytes = 0;
    while (1) {
        size_t free_bytes = xRingbufferGetCurFreeSize(audio_rb);
        float fill = 100.0f * (1.0f - ((float)free_bytes / AUDIO_RINGBUF_SIZE));
        size_t heap_free = xPortGetFreeHeapSize();

        uint32_t frames_delta = frames_sent - last_frames;
        size_t bytes_delta = total_bytes_sent - last_bytes;

        ESP_LOGI(TAG,
            "[Health] rb_fill=%.1f%% free=%u heap=%u frames/s=%u bytes/s=%u",
            fill, (unsigned)free_bytes, (unsigned)heap_free,
            (unsigned)(frames_delta / 5), (unsigned)(bytes_delta / 5));

        last_frames = frames_sent;
        last_bytes = total_bytes_sent;

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ---------- Accurate CPU profiler (using FreeRTOS runtime stats) ---------- */
static void cpu_profiler_task(void *arg) {
    static char stats_buf[2048];
    const TickType_t delay = pdMS_TO_TICKS(5000);

    for (;;) {
        vTaskDelay(delay);

        // get full run-time stats table
        memset(stats_buf, 0, sizeof(stats_buf));
        vTaskGetRunTimeStats(stats_buf);

        // parse total and IDLE time
        uint64_t total_time = 0;
        uint64_t idle_time = 0;
        char *line = strtok(stats_buf, "\n");
        while (line) {
            char name[32];
            uint64_t time = 0;
            if (sscanf(line, "%31s %llu", name, &time) == 2) {
                total_time += time;
                if (strcmp(name, "IDLE") == 0)
                    idle_time = time;
            }
            line = strtok(NULL, "\n");
        }

        float idle_ratio = 0.0f, load_ratio = 0.0f;
        if (total_time > 0) {
            idle_ratio = (float)idle_time / (float)total_time * 100.0f;
            load_ratio = 100.0f - idle_ratio;
        }

        ESP_LOGI("CPU", "Idle: %.1f%%   Load: %.1f%%", idle_ratio, load_ratio);
    }
}

static void heap_watchdog_task(void *arg)
{
    size_t prev_free = xPortGetFreeHeapSize();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200)); // sample every 200ms

        // detect which task is active
        TaskHandle_t cur = xTaskGetCurrentTaskHandle();
        const char *tname = pcTaskGetName(cur);

        size_t now_free = xPortGetFreeHeapSize();
        if (tname && strcmp(tname, "esp_timer") == 0) {
            if (now_free != prev_free) {
                ets_printf("⚠️ esp_timer changed heap: free %u -> %u bytes\n",
                           (unsigned)prev_free, (unsigned)now_free);
            }
        }

        prev_free = now_free;
    }
}

void dump_all_timers(void)
{
    printf("\n\n==== ACTIVE ESP_TIMERS ====\n");
    esp_timer_dump(stdout);
    printf("===========================\n\n");
}

static void i2s_test_task(void *arg) {
    esp_task_wdt_add(NULL);
    int32_t buf[256];
    size_t bytes_read;
    while (1) {
        esp_err_t ret = i2s_channel_read(rx_chan, buf, sizeof(buf), &bytes_read, pdMS_TO_TICKS(100));
        esp_task_wdt_reset();
        if (ret == ESP_OK && bytes_read > 0) {
            ESP_LOGI(TAG, "I2S OK: read %u bytes", bytes_read);
        } else {
            ESP_LOGW(TAG, "I2S timeout or fail ret=%d", ret);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- app_main ---------- */
void app_main(void) {
    esp_log_level_set("NimBLE", ESP_LOG_ERROR);
    //esp_log_level_set("*", ESP_LOG_NONE);
    ESP_LOGI(TAG, "Speechster B1 Starting Up");

    state_lock = xSemaphoreCreateMutex();
    audio_rb = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!audio_rb) {
        ESP_LOGE(TAG, "Failed to create Ringbuffer, audio_rb: %d", audio_rb);
        return;
    } else {
        ESP_LOGI(TAG, "audio_rb: %d", audio_rb);
    }

    ringbuf_mutex = xSemaphoreCreateMutex();
    if (!ringbuf_mutex) {
        ESP_LOGE(TAG, "Failed to create ringbuf_mutex");
        return;
    }

    // ADC (legacy call) - acceptable here.
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(FSR_ADC_CHANNEL, ADC_ATTEN_DB_12);

    // I2S
    i2s_init_inmp441();

    // Start ctrl_queue early
    ctrl_queue = xQueueCreate(CTRL_QUEUE_LEN, sizeof(control_msg_t));

    if (!ctrl_queue) {
        ESP_LOGE(TAG, "Failed to create ctrl_queue");
        return;
    }

    xTaskCreate(control_task, "ctrl_task", 4096, NULL, 5, NULL);

    // NimBLE
    init_nimble();

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
    xTaskCreate(i2s_capture_task, "i2s_cap", 16*1024, NULL, 4, NULL);
    xTaskCreate(fsr_task, "fsr", 4*1024, NULL, 3, NULL);
    xTaskCreate(ble_sender_task, "ble_send", 24*1024, NULL, 5, NULL);
    //xTaskCreate(heap_watchdog_task, "heapwatch", 4096, NULL, 1, NULL);

    //xTaskCreate(mic_test_task, "mic_test", 4096, NULL, 5, NULL);
    // xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 1, NULL);

    xTaskCreatePinnedToCore(
        telemetry_task, "telemetry", 4096, NULL, 1, NULL, 0
    );

    // xTaskCreate(cpu_profiler_task, "cpu_prof", 6144, NULL, 1, NULL);

    ESP_LOGI(TAG, "Main started; advertising automatically, UART logs enabled");

    vTaskDelay(pdMS_TO_TICKS(100));  // let tasks start
}