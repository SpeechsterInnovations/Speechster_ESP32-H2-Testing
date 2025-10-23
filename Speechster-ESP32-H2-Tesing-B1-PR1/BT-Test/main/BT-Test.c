// ─────────────────────────────────────────────
// Speechster-H2 BLE Audio Streamer — v1.0 STABLE
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
#include <math.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_timer.h"

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

static const char *TAG = "speechster_b1";

/* ---------- Defaults & config ---------- */
#define UART_BAUD_RATE       115200
#define AUDIO_SAMPLE_RATE    48000
#define AUDIO_FRAME_MS       20
#define AUDIO_FRAME_SAMPLES   ((AUDIO_SAMPLE_RATE / 1000) * AUDIO_FRAME_MS)  // 960 at 48kHz
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_READ_BYTES     (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_RINGBUF_SIZE   (48 * 1024)
#define STREAM_SLICE_MAX   (8 * 1024) // Smaller slice size reduces total number of chunks per logical frame
#define I2S_BUF_SIZE 1024
#define LOG_CTRL(fmt, ...) ESP_LOGI(TAG, "[CTRL_IN] " fmt, ##__VA_ARGS__)
#define LOG_JSON_PARSE(fmt, ...) ESP_LOGI(TAG, "[JSON_PARSE] " fmt, ##__VA_ARGS__)
#define LOG_STREAM(fmt, ...) ESP_LOGI(TAG, "[STREAM_OUT] " fmt, ##__VA_ARGS__)
static i2s_chan_handle_t rx_chan = NULL; 
static uint32_t rb_overflow_count = 0;
static uint32_t ble_delay_ms = 15;

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


/* runtime state */
static const uint8_t AUDIO_FLAG_PCM = 0x00;
static volatile bool mic_enabled = false;
static volatile bool fsr_enabled = false;
static uint16_t seq_counter = 0;
static volatile bool bt_connected = false;
static uint8_t *agg_buf = NULL;

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

/* ---------- I2S (INMP441) ---------- */
static void i2s_init_inmp441(void) {
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


void mic_test_task(void *param) {
    mic_enabled = true;

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

/* ---------- Heap-safe pre-allocated buffers ---------- */
#define MAX_FRAME_SIZE   (8 + AUDIO_READ_BYTES)  // 8-byte header + PCM
static uint8_t audio_frame_buf[MAX_FRAME_SIZE]; // pre-allocated buffer for PCM frames

/* ---------- I2S capture task (heap-safe) ---------- */
static void i2s_capture_task(void *arg) {
    // Now capturing at 48 kHz, but downsampling to 16 kHz PCM16 before BLE send.
    // Effective BLE audio bandwidth ~32 kB/s (≈16 kHz mono).

    ESP_LOGI(TAG, "i2s_capture_task start");

    const uint32_t frame_ms = 5;  // capture 5 ms slices instead of 20 ms
    const size_t frame_samples = (AUDIO_SAMPLE_RATE / 1000) * frame_ms;
    const size_t pcm_bytes = frame_samples * sizeof(int16_t);
    const size_t frame_sz = 8 + pcm_bytes;

    // heap-allocate buffers to avoid stack overflow in FreeRTOS tasks
    int32_t *i2s_buf = (int32_t*) pvPortMalloc(frame_samples * sizeof(int32_t));
    int16_t *pcm_down = (int16_t*) pvPortMalloc((frame_samples / 3 + 4) * sizeof(int16_t));
    if (!i2s_buf || !pcm_down) {
        ESP_LOGE(TAG, "i2s_capture_task: buffer alloc failed");
        if (i2s_buf) vPortFree(i2s_buf);
        if (pcm_down) vPortFree(pcm_down);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (audio_rb == NULL) {
            ESP_LOGW(TAG, "audio_rb not ready yet");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!mic_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Backoff dynamically if ringbuffer nearly full
        size_t free_bytes = xRingbufferGetCurFreeSize(audio_rb);
        float fill_ratio = 1.0f - ((float)free_bytes / (float)AUDIO_RINGBUF_SIZE);
        if (fill_ratio > 0.85f) {
            vTaskDelay(pdMS_TO_TICKS(50)); // pause 50 ms to let BLE drain
            taskYIELD();
        }


        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_chan, i2s_buf,
                                         frame_samples * sizeof(int32_t),
                                         &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TAG, "I2S read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t samples_read = bytes_read / sizeof(int32_t);

        if (free_bytes < AUDIO_READ_BYTES * 2) {
            vTaskDelay(pdMS_TO_TICKS(ble_delay_ms)); // brief pause if buffer nearly full
            taskYIELD();
            continue;
        }

        // Downsample 48 kHz → 16 kHz by simple decimation (every 3rd sample).
        // INMP441 has an internal low-pass filter, so no FIR filter needed here.
        size_t j = 0;
        for (size_t i = 0; i + 2 < samples_read; i += 3) {
            int32_t s = (i2s_buf[i] >> 8);  // take one out of every 3 samples
            pcm_down[j++] = (int16_t)s;
        }
        size_t down_samples = j;
        size_t down_bytes = down_samples * sizeof(int16_t);

        uint8_t flags = AUDIO_FLAG_PCM;
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint16_t seq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);


        pack_header(audio_frame_buf, 0x01, flags, seq, ts);
        memcpy(audio_frame_buf + 8, pcm_down, down_bytes);

        if (xRingbufferSend(audio_rb, audio_frame_buf, 8 + down_bytes, pdMS_TO_TICKS(20)) != pdTRUE) {
            rb_overflow_count++;
        }

        if (free_bytes < AUDIO_READ_BYTES * 4) {
            // back off capture when BLE can't drain fast enough
            vTaskDelay(pdMS_TO_TICKS(ble_delay_ms * 2));
        }

        // Optional debug — verify frames are capped
        // ESP_LOGI(TAG, "Captured frame: %zu bytes (capped=%zu)", pcm_bytes, capped_bytes);

        static uint32_t last_wdt = 0;
        uint32_t now = esp_log_timestamp();
        if (now - last_wdt > 2000) { // feed every 2s
            last_wdt = now;
        }

        // pace capture so BLE sender keeps up
        vTaskDelay(pdMS_TO_TICKS(frame_ms * 2));  // slows capture ~2×
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(2)); // allow timer_task to run
    }

    // (not reached in normal operation)
    vPortFree(i2s_buf);
    vPortFree(pcm_down);
    vTaskDelete(NULL);
}

/* ---------- FSR ADC polling ---------- */
static void fsr_task(void *arg) {
    ESP_LOGI(TAG, "fsr_task start (ADC channel %d)", FSR_ADC_CHANNEL);
    while (1) {
        if (!fsr_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int raw = adc1_get_raw(FSR_ADC_CHANNEL);
        uint8_t payload[2];
        payload[0] = raw & 0xff;
        payload[1] = (raw >> 8) & 0xff;
        size_t frame_sz = 8 + sizeof(payload);
        uint8_t *frame = (uint8_t*) pvPortMalloc(frame_sz);
        if (!frame) { vTaskDelay(pdMS_TO_TICKS(fsr_interval_ms)); continue; }
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint16_t seq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
        pack_header(frame, 0x02, 0x00, seq, ts);
        memcpy(frame + 8, payload, sizeof(payload));
        if (xRingbufferSend(audio_rb, frame, frame_sz, pdMS_TO_TICKS(50)) != pdTRUE) {
            vPortFree(frame);
        }
        vTaskDelay(pdMS_TO_TICKS(fsr_interval_ms));
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
                    mic_enabled = cJSON_IsTrue(jmic);
                    LOG_JSON_PARSE("mic=%d", mic_enabled);
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


// -----------------------------------------------------------------------------
//  send_chunked_notify()  --  Adaptive BLE chunked sender with pacing auto-tune
// -----------------------------------------------------------------------------
static void send_chunked_notify(uint16_t conn_handle,
                                uint16_t attr_handle,
                                const uint8_t *frame,
                                size_t frame_len)
{
    if (!bt_connected || attr_handle == 0) {
        ESP_LOGW(TAG, "[SEND_CHUNKED_NOTIFY]: no active BLE client (conn=%d, attr=%d)",
                 conn_handle, attr_handle);
        return;
    }

    static uint32_t adaptive_delay_ms = 20;  // start slightly slower
    const uint32_t delay_min = 10;
    const uint32_t delay_max = 60;

    /* Determine max payload from MTU */
    uint16_t cur_mtu = ble_att_mtu(conn_handle);
    size_t payload_max = 120;
    if (cur_mtu > 3) {
        size_t candidate = (size_t)cur_mtu - 3;
        if (candidate > 200) candidate = 200;   // cap smaller to reduce chunk count
        payload_max = candidate;
    }
    if (payload_max < 64) payload_max = 64;

    /* Safety clamp */
    if (frame_len > STREAM_SLICE_MAX) {
        ESP_LOGI(TAG, "Frame sliced (%zu → %d bytes)", frame_len, STREAM_SLICE_MAX);
        frame_len = STREAM_SLICE_MAX;
    }

    bool first_chunk = true;
    size_t pos = 0;

    /* Adaptive pacing variables */
    uint32_t backoff_ms = adaptive_delay_ms;
    const uint32_t backoff_max = 400;
    const uint32_t backoff_multiplier = 2;
    uint32_t success_count = 0, fail_count = 0;
    static int consecutive_failures = 0;

    while (pos < frame_len) {
        size_t remaining = frame_len - pos;
        size_t take = (remaining > payload_max) ? payload_max : remaining;

        uint8_t notify_buf[260];
        memcpy(notify_buf, frame, 8);
        if (!first_chunk || (pos + take) < frame_len)
            notify_buf[1] |= 0x02;  // continuation flag

        memcpy(notify_buf + 8, frame + pos, take);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(notify_buf, 8 + take);
        if (!om) {
            // mbuf pool exhausted — abort sending this aggregate and back off.
            ESP_LOGW(TAG, "mbuf exhausted while sending seq=%u pos=%zu — abort aggregate, backing off %ums",
                    /* seq */ (unsigned) ((frame_len >= 8) ? frame[2] | (frame[3]<<8) : 0),
                    pos, backoff_ms);
            // Cleanly drop the partially-sent aggregate (or you could requeue it).
            // Back off before returning to main loop so the BLE stack recovers.
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            // increase backoff but cap it
            backoff_ms = backoff_ms < backoff_max ? backoff_ms * backoff_multiplier : backoff_max;
            return; // abort sending the rest of this frame — caller will decide next action
        }

        int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
    
        if (rc == 0) {
            success_count++;
            consecutive_failures = 0;
            backoff_ms = adaptive_delay_ms;

            if ((success_count % 128) == 0) {
                ESP_LOGI(TAG, "Notify OK (rb_free=%zu) mtu=%u payload=%zu delay=%ums",
                         xRingbufferGetCurFreeSize(audio_rb), cur_mtu, take, adaptive_delay_ms);
            }
        } else {
            fail_count++;
            consecutive_failures++;
            ESP_LOGW(TAG, "Notify failed rc=%d (take=%zu pos=%zu/%zu) — backoff %ums",
                     rc, take, pos + take, frame_len, backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = MIN(backoff_ms * backoff_multiplier, backoff_max);

            // Detect link loss or persistent failure
            if (rc == BLE_HS_ENOTCONN || consecutive_failures > 10) {
                ESP_LOGE(TAG, "BLE link lost (rc=%d) — forcing disconnect", rc);
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                bt_connected = false;
                g_conn_handle = 0;
                g_stream_attr_handle = 0;
                consecutive_failures = 0;
                return;
            }
            continue;
        }

        pos += take;
        first_chunk = false;

        /* === Auto-tune pacing based on success ratio === */
        if ((success_count + fail_count) >= 64) {
            float success_ratio = (float)success_count / (success_count + fail_count);
            if (success_ratio < 0.80f && adaptive_delay_ms < delay_max) {
                adaptive_delay_ms += 2;  // slow down a bit
            } else if (success_ratio > 0.95f && adaptive_delay_ms > delay_min) {
                adaptive_delay_ms -= 1;  // speed up slightly
            }
            success_count = fail_count = 0;
            ble_delay_ms = adaptive_delay_ms;  // update global pacing
        }

        vTaskDelay(pdMS_TO_TICKS(adaptive_delay_ms));
        taskYIELD();
    }
}

/* ---------- Pre-allocated notification buffer ---------- */
#define MAX_NOTIFY_BUF  260
static uint8_t notify_buf[MAX_NOTIFY_BUF];

/* ---------- Heap-free BLE sender task ---------- */
static void ble_sender_task(void *arg)
{
    ESP_LOGI(TAG, "ble_sender_task start");

    static uint32_t last_wdt = 0;

    // Aggregate buffer: 8 KB payload + 8-byte header + 4-byte footer
    #define AGGREGATE_BYTES  (18 * 1024)
    uint8_t *cursor = agg_buf + 8;
    size_t agg_used = 0;
    uint32_t ts = 0;
    uint16_t seq = 0;

    while (1) {
        taskYIELD();

        // Skip work when BLE is not connected
        if (!bt_connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // --- Drain any pending audio data when mic is disabled ---
        if (!mic_enabled) {
            uint8_t *drop_item;
            size_t drop_size;

            while ((drop_item = (uint8_t *)xRingbufferReceive(audio_rb, &drop_size, 0))) {
                vRingbufferReturnItem(audio_rb, drop_item);
            }

            agg_used = 0;  // reset aggregation state
            vTaskDelay(pdMS_TO_TICKS(200));  // gentle idle pacing
            continue;  // skip sending loop
        }

        uint8_t *item = NULL;
        size_t item_size = 0;

        /* --- Fake mode --- */
        if (fake_mode && g_stream_attr_handle > 0 && bt_connected) {
            uint8_t fake_payload[4] = { 0x11, 0x22, 0x33, 0x44 };
            uint8_t fake_frame[8 + sizeof(fake_payload)];
            uint32_t fts = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint16_t fseq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
            pack_header(fake_frame, 0x01, 0x00, fseq, fts);
            memcpy(fake_frame + 8, fake_payload, sizeof(fake_payload));
            send_chunked_notify(g_conn_handle, g_stream_attr_handle, fake_frame, sizeof(fake_frame));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* --- Real audio aggregation --- */
        item = (uint8_t *)xRingbufferReceive(audio_rb, &item_size, pdMS_TO_TICKS(50));

        if (item && item_size >= 8) {
            if (agg_used == 0) {
                // start a new aggregate frame
                ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
                seq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
                cursor = agg_buf + 8;
                agg_used = 0;
            }

            // skip 8-byte inner header; only append PCM payload
            size_t payload_len = item_size - 8;

            // if adding this would exceed the aggregate buffer, flush current batch first
            if (agg_used + payload_len > AGGREGATE_BYTES) {
                uint16_t footer_seq = seq;
                uint16_t footer_crc = calc_checksum16(agg_buf + 8, agg_used);

                memcpy(agg_buf + 8 + agg_used, &footer_seq, sizeof(footer_seq));
                memcpy(agg_buf + 8 + agg_used + sizeof(footer_seq), &footer_crc, sizeof(footer_crc));

                size_t total_len = 8 + agg_used + 4; // header + payload + footer
                pack_header(agg_buf, 0x01, AUDIO_FLAG_PCM, seq, ts);
                send_chunked_notify(g_conn_handle, g_stream_attr_handle, agg_buf, total_len);

                agg_used = 0;
                cursor = agg_buf + 8;
                ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
                seq++;
            }

            if (agg_used + payload_len <= AGGREGATE_BYTES) {
                memcpy(cursor, item + 8, payload_len);
                cursor += payload_len;
                agg_used += payload_len;
            } else {
                ESP_LOGI(TAG, "Aggregate overflow prevented (%u+%u > %u)",
                        (unsigned)agg_used, (unsigned)payload_len, (unsigned)AGGREGATE_BYTES);
                agg_used = AGGREGATE_BYTES; // force flush next loop
            }


            vRingbufferReturnItem(audio_rb, item);
            item = NULL;
        } else {
            // timeout or no data → flush whatever aggregate exists
            if (agg_used > 0 && g_stream_attr_handle > 0 && bt_connected) {
                uint16_t footer_seq = seq;
                uint16_t footer_crc = calc_checksum16(agg_buf + 8, agg_used);

                memcpy(agg_buf + 8 + agg_used, &footer_seq, sizeof(footer_seq));
                memcpy(agg_buf + 8 + agg_used + sizeof(footer_seq), &footer_crc, sizeof(footer_crc));

                size_t total_len = 8 + agg_used + 4;
                pack_header(agg_buf, 0x01, AUDIO_FLAG_PCM, seq, ts);
                send_chunked_notify(g_conn_handle, g_stream_attr_handle, agg_buf, total_len);

                agg_used = 0;
                cursor = agg_buf + 8;
            }
            else if (item && item_size < 8) {
                // only return if not processed
                vRingbufferReturnItem(audio_rb, item);
                item = NULL;
            }

            // no active client or idle → small sleep
            vTaskDelay(pdMS_TO_TICKS(ble_delay_ms));
        }

        /* Feed WDT and pacing */
        uint32_t now = esp_log_timestamp();
        if (now - last_wdt > 2000) {
            last_wdt = now;
        }

        UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
        if (watermark < 512) {
            ESP_LOGW(TAG, "[STACK] ble_sender low watermark: %u words", watermark);
        }

        // mild pacing between aggregate flushes
        vTaskDelay(pdMS_TO_TICKS(ble_delay_ms * 2));
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
                bt_connected = true;
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

            } else {
                ESP_LOGW(TAG, "Connect failed; status=%d", event->connect.status);
                // Restart advertising
                ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "Client disconnected (reason=%d)", event->disconnect.reason);

            // Clear runtime connection state
            bt_connected = false;
            g_stream_attr_handle = 0;

            // Allow NimBLE a short window to finalize cleanup
            vTaskDelay(pdMS_TO_TICKS(200));

            // Restart advertising automatically
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

static void telemetry_task(void *arg) {
    while (1) {
        size_t free_bytes = xRingbufferGetCurFreeSize(audio_rb);
        float fill = 100.0f * (1.0f - ((float)free_bytes / AUDIO_RINGBUF_SIZE));
        UBaseType_t waiting_items = uxQueueMessagesWaiting(audio_rb);
        ESP_LOGI(TAG, "[Health] rb_fill=%.1f%% free=%u waiting=%u", fill, free_bytes, waiting_items);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


/* ---------- app_main ---------- */
void app_main(void) {

    esp_log_level_set("NimBLE", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "Speechster B1 Starting Up");

    state_lock = xSemaphoreCreateMutex();
    audio_rb = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!audio_rb) {
        ESP_LOGE(TAG, "Failed to create Ringbuffer, audio_rb: %d", audio_rb);
        return;
    } else {
        ESP_LOGI(TAG, "audio_rb: %d", audio_rb);
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

    agg_buf = heap_caps_malloc(8 + AGGREGATE_BYTES + 4, MALLOC_CAP_8BIT);
    if (!agg_buf) {
        ESP_LOGE(TAG, "Failed to allocate agg_buf");
        return;
    }
    memset(agg_buf, 0, 8 + AGGREGATE_BYTES + 4);

    // Start Tasks
    xTaskCreate(i2s_capture_task, "i2s_cap", 8*1024, NULL, 6, NULL);
    xTaskCreate(fsr_task, "fsr", 4*1024, NULL, 5, NULL);
    xTaskCreate(ble_sender_task, "ble_send", 20*1024, NULL, 3, NULL);

    //xTaskCreate(mic_test_task, "mic_test", 4096, NULL, 5, NULL);
    // xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 1, NULL);

    xTaskCreatePinnedToCore(
        telemetry_task, "telemetry", 4096, NULL, 1, NULL, 0
    );


    ESP_LOGI(TAG, "Main started; advertising automatically, UART logs enabled");

    vTaskDelay(pdMS_TO_TICKS(100));  // let tasks start
}