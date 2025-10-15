// main.c
// ESP-IDF v5.5.1 (dirty) - NimBLE GATT server with JSON control + stream notify
// INMP441 I2S pins: SD=GPIO10, SCK=GPIO3, WS=GPIO14 (L/R tied to GND -> left channel)
// FSR402 ADC: ADC1 channel 1
// UART logging @115200, advertise automatically.
// ADPCM encoder included (ima adpcm) and selectable at runtime via JSON control.
// cJSON is used for control parsing (available in ESP-IDF).
//
// Build:
//   idf.py set-target esp32h2
//   idf.py menuconfig  -> enable NimBLE (component config), set MTU if desired
//   idf.py build
//   idf.py -p /dev/ttyACM0 flash monitor

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

/* ADPCM header (drop-in) */
#include "adpcm.h"

static const char *TAG = "speechster_h2";

/* ---------- Defaults & config ---------- */
#define UART_BAUD_RATE       115200
#define AUDIO_SAMPLE_RATE    48000
#define AUDIO_FRAME_MS       20
#define AUDIO_FRAME_SAMPLES   ((AUDIO_SAMPLE_RATE / 1000) * AUDIO_FRAME_MS)  // 960 at 48kHz
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_READ_BYTES     (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_RINGBUF_SIZE   (64 * 1024)
#define I2S_BUF_SIZE 1024
#define LOG_LOCAL_LEVEL ESP_LOG_WARN
static i2s_chan_handle_t rx_chan = NULL; 
static uint32_t rb_overflow_count = 0;

#define I2S_PORT_NUM         I2S_NUM_0
#define I2S_BCK_IO           3
#define I2S_WS_IO            14
#define I2S_DATA_IN_IO       10
#define I2S_NUM         I2S_NUM_0
#define SAMPLE_BUF_LEN  320  // Number of samples per read


#define FSR_ADC_CHANNEL      ADC_CHANNEL_1
#define CONSERVATIVE_LL_OCTETS 220
static uint32_t fsr_interval_ms = 50; // default 50ms, can be changed via JSON
adpcm_state_t adpcm_state;

/* BLE custom UUIDs (browser should use the same strings when connecting) */
static const char *SERVICE_UUID_STR       = "0000feed-0000-1000-8000-00805f9b34fb";
static const char *CHAR_CONTROL_UUID_STR = "0000feed-0001-0000-8000-00805f9b34fb";
static const char *CHAR_STREAM_UUID_STR  = "0000feed-0002-0000-8000-00805f9b34fb";

/* Packet framing (8 byte header):
 * 0: type (0x01 audio, 0x02 sensor)
 * 1: flags (bit0 = compressed ADPCM)
 * 2-3: seq (uint16 little-endian)
 * 4-7: ts_ms (uint32 little-endian)
 * payload follows
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

/* runtime state */
typedef enum { ENC_PCM = 0, ENC_ADPCM = 1 } encoder_t;
static encoder_t default_encoder = ENC_PCM; // default used when JSON omits encoder
static encoder_t current_encoder = ENC_PCM; // active encoder (settable via JSON)
static volatile bool mic_enabled = false;
static volatile bool fsr_enabled = false;
static uint16_t seq_counter = 0;

static RingbufHandle_t audio_rb = NULL;
static SemaphoreHandle_t state_lock = NULL;

/* BLE state */
static uint16_t g_conn_handle = 0;
static uint16_t g_stream_attr_handle = 0; // populated on subscribe
static volatile bool fake_mode = false; // start with fake frames disabled

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
static uint8_t adpcm_buf[AUDIO_READ_BYTES/2 + 16]; // max ADPCM output size

/* ---------- I2S capture task (heap-safe) ---------- */
static void i2s_capture_task(void *arg)
{
    ESP_LOGI(TAG, "i2s_capture_task start");

    const uint32_t frame_ms = 10;  // capture 10 ms slices instead of 20 ms
    const size_t frame_samples = (AUDIO_SAMPLE_RATE / 1000) * frame_ms;
    const size_t pcm_bytes = frame_samples * sizeof(int16_t);
    const size_t frame_sz = 8 + pcm_bytes;

    int32_t i2s_buf[frame_samples];
    int16_t pcm_frame[frame_samples];

    while (1) {
        if (!mic_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
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

        size_t free_bytes = xRingbufferGetCurFreeSize(audio_rb);
        if (free_bytes < AUDIO_READ_BYTES * 2) {
            vTaskDelay(pdMS_TO_TICKS(10)); // brief pause if buffer nearly full
            continue;
        }

        size_t samples_read = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < samples_read; i++) {
            pcm_frame[i] = (int16_t)(i2s_buf[i] >> 8);
        }

        uint8_t flags = (current_encoder == ENC_ADPCM) ? 0x01 : 0x00;
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint16_t seq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
        pack_header(audio_frame_buf, 0x01, flags, seq, ts);
        memcpy(audio_frame_buf + 8, pcm_frame, pcm_bytes);

        if (xRingbufferSend(audio_rb, audio_frame_buf, frame_sz, pdMS_TO_TICKS(20)) != pdTRUE) {
            rb_overflow_count++;
        }

        // pace capture so BLE sender keeps up
        vTaskDelay(pdMS_TO_TICKS(frame_ms / 2));  // 5 ms pause between reads
    }
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

/* --- Logging macros --- */
#define LOG_CTRL(fmt, ...) ESP_LOGI(TAG, "[CTRL_IN] " fmt, ##__VA_ARGS__)
#define LOG_JSON_PARSE(fmt, ...) ESP_LOGI(TAG, "[JSON_PARSE] " fmt, ##__VA_ARGS__)
#define LOG_STREAM(fmt, ...) ESP_LOGI(TAG, "[STREAM_OUT] " fmt, ##__VA_ARGS__)

/* ---------- GATT: control (write JSON) ---------- */
static int gatt_control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0) return 0;
    char *json = (char*) malloc(len + 1);
    if (!json) return 0;
    os_mbuf_copydata(ctxt->om, 0, len, json);
    json[len] = '\0';

    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
    LOG_CTRL("ts=%u raw=%s", ts, json);

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "[CTRL_ERR] Invalid JSON");
        free(json);
        return 0;
    }

    xSemaphoreTake(state_lock, pdMS_TO_TICKS(200));

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

    cJSON *jenc = cJSON_GetObjectItem(root, "encoder");
    if (jenc && cJSON_IsString(jenc)) {
        if (strcasecmp(jenc->valuestring, "pcm") == 0) {
            current_encoder = ENC_PCM;
            LOG_JSON_PARSE("encoder=PCM");
        } else if (strcasecmp(jenc->valuestring, "adpcm") == 0) {
            current_encoder = ENC_ADPCM;
            adpcm_reset_state(&adpcm_state);
            LOG_JSON_PARSE("encoder=ADPCM");
        }
    }

    cJSON *jint = cJSON_GetObjectItem(root, "fsr_interval_ms");
    if (jint && cJSON_IsNumber(jint)) {
        uint32_t v = (uint32_t) jint->valuedouble;
        if (v >= 10 && v <= 5000) {
            fsr_interval_ms = v;
            LOG_JSON_PARSE("fsr_interval_ms=%u", fsr_interval_ms);
        }
    }

    cJSON *jfake = cJSON_GetObjectItem(root, "fake");
    if (jfake && cJSON_IsBool(jfake)) {
        fake_mode = cJSON_IsTrue(jfake);
        LOG_JSON_PARSE("fake_mode=%d", fake_mode);
    }


    xSemaphoreGive(state_lock);
    cJSON_Delete(root);
    free(json);
    return 0;
}

/* helper */

static void send_chunked_notify(uint16_t conn_handle, uint16_t attr_handle,
                                uint8_t *frame, size_t frame_len)
{

    static uint32_t last_notify_time_us = 0;
    static float avg_notify_interval_us = 8000.0f; // initial guess ~8ms
    const float alpha = 0.1f;                      // smoothing factor

    ESP_LOGI(TAG, "send_chunked_notify(conn=%d, attr=%d, frame_len=%zu)",
             conn_handle, attr_handle, frame_len);

    if (attr_handle == 0) {
        ESP_LOGW(TAG, "Notify skipped (invalid handle)");
        return;
    }

    uint16_t att_mtu = ble_att_mtu(conn_handle);
    if (att_mtu == 0) att_mtu = 23;
    size_t att_payload_max = (att_mtu > 3) ? (att_mtu - 3) : 20;
    size_t ll_payload_max = CONSERVATIVE_LL_OCTETS;
    size_t payload_max = (att_payload_max < ll_payload_max) ? att_payload_max : ll_payload_max;
    if (payload_max > 8) payload_max -= 8; else payload_max = 8;

    size_t pos = 0;
    bool first_chunk = true;

    while (pos < frame_len) {
    size_t remaining = frame_len - pos;
    size_t take = (remaining > payload_max) ? payload_max : remaining;

    // ESP_LOGI(TAG, "Chunk: take=%zu pos=%zu/%zu payload_max=%zu", take, pos, frame_len, payload_max);

    uint8_t notify_buf[260];
    memcpy(notify_buf, frame, 8);

    if (!first_chunk)
        notify_buf[1] = 0x02;                   // mid-chunk flag
    else if (pos + take < frame_len)
        notify_buf[1] |= 0x02;                 // start but not last

    memcpy(notify_buf + 8, frame + pos, take);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(notify_buf, 8 + take);
    if (om) {
        int rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
        if (rc == 0) {
            static uint32_t notify_count = 0;
            if ((notify_count++ % 128) == 0) {  // log every 128th notify only
                ESP_LOGI(TAG, "Notify OK (avg=%.1f us, rb_free=%zu)",
                        avg_notify_interval_us, xRingbufferGetCurFreeSize(audio_rb));
            }
                    if (rc == 0) {
            uint32_t now = (uint32_t)esp_timer_get_time();
            if (last_notify_time_us > 0) {
                float delta = (float)(now - last_notify_time_us);
                avg_notify_interval_us = (alpha * delta) + ((1.0f - alpha) * avg_notify_interval_us);
            }
            last_notify_time_us = now;

            // Dynamic delay based on observed notify speed
            uint32_t target_delay_us = (uint32_t)(avg_notify_interval_us * 0.9f);
            if (target_delay_us < 3000) target_delay_us = 3000;   // lower bound 3ms
            if (target_delay_us > 20000) target_delay_us = 20000; // upper bound 20ms

            esp_rom_delay_us(target_delay_us); // fine-grained pacing (microsecond precision)

            // --- 🔹 Ringbuffer-aware pacing ---
            size_t free_bytes = xRingbufferGetCurFreeSize(audio_rb);
            size_t total_size = AUDIO_RINGBUF_SIZE;
            float fill_ratio = 1.0f - ((float)free_bytes / (float)total_size);

            // If ring buffer >75% full, increase pacing delay slightly (slow down)
            if (fill_ratio > 0.75f) {
                uint32_t extra_delay_us = (uint32_t)(1000 * (fill_ratio - 0.75f) * 20); // up to +5ms
                esp_rom_delay_us(extra_delay_us);
                ESP_LOGD(TAG, "[Congestion] rb_fill=%.1f%% (+%u us)", fill_ratio * 100.0f, extra_delay_us);
            }
            // If buffer nearly empty (<25%), decrease pacing (speed up)
            else if (fill_ratio < 0.25f) {
                ESP_LOGI(TAG, "[FlowBoost] rb_fill=%.1f%% (fast path)", fill_ratio * 100.0f);
                esp_rom_delay_us(1000); // small minimal delay for smoothness
            }

        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); // fallback if BLE busy
        }

        } else {
            ESP_LOGW(TAG, "Notify failed rc=%d (take=%zu pos=%zu/%zu)", rc, take, pos + take, frame_len);
            vTaskDelay(pdMS_TO_TICKS(10));      // back off slightly if failed
        }
    } else {
        ESP_LOGE(TAG, "Failed to alloc mbuf for notify chunk (%zu bytes)", 8 + take);
        vTaskDelay(pdMS_TO_TICKS(5));           // let memory recover
    }

    pos += take;
    first_chunk = false;
}


    ESP_LOGI(TAG, "send_chunked_notify() done");
}



/* ---------- BLE sender: hybrid fake + real streaming ---------- */

/* ---------- Pre-allocated notification buffer ---------- */
#define MAX_NOTIFY_BUF  260
static uint8_t notify_buf[MAX_NOTIFY_BUF];

/* ---------- Heap-free BLE sender task ---------- */
static void ble_sender_task(void *arg) {
    ESP_LOGI(TAG, "ble_sender_task start");

    while (1) {
        /* --- Fake frames --- */
        if (fake_mode && g_stream_attr_handle > 0) {
            uint8_t fake_payload[4] = { 0x11, 0x22, 0x33, 0x44 };
            uint8_t fake_frame[8 + sizeof(fake_payload)];
            uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint16_t seq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
            pack_header(fake_frame, 0x01, 0x00, seq, ts);
            memcpy(fake_frame + 8, fake_payload, sizeof(fake_payload));
            send_chunked_notify(g_conn_handle, g_stream_attr_handle, fake_frame, sizeof(fake_frame));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* --- Real audio frames --- */
        size_t item_size;
        uint8_t *item = (uint8_t*) xRingbufferReceive(audio_rb, &item_size, pdMS_TO_TICKS(500));
        if (!item) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        if (g_stream_attr_handle > 0) {
            ESP_LOGI(TAG, "Streaming frame (%zu bytes)...", item_size);
            // Use chunked notify for large audio frames
            send_chunked_notify(g_conn_handle, g_stream_attr_handle, item, item_size);
        }

        vRingbufferReturnItem(audio_rb, item);
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

            } else {
                ESP_LOGW(TAG, "Connect failed; status=%d", event->connect.status);
                // Restart advertising
                ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
            g_conn_handle = 0;
            g_stream_attr_handle = 0;
            // Restart advertising
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
            } else {
                if (g_stream_attr_handle == event->subscribe.attr_handle) {
                    g_stream_attr_handle = 0;
                }
            }

        if (g_stream_attr_handle > 0) {
            // Defer test notify to allow NimBLE connection context to stabilize
            uint8_t test_frame[16];
            for (int i=0; i<16; i++) test_frame[i] = i;

            ESP_LOGI(TAG, "Scheduling delayed test notification...");
            vTaskDelay(pdMS_TO_TICKS(1000));  // wait 1s after subscribe
            ESP_LOGI(TAG, "Attempting notify on conn=%d attr=%d", g_conn_handle, g_stream_attr_handle);

            send_chunked_notify(g_conn_handle, g_stream_attr_handle, test_frame, sizeof(test_frame));
            ESP_LOGI(TAG, "Test notification sent");
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

    const char *name = "Speechster-H2";
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
    ble_svc_gap_device_name_set("Speechster-H2");


    // Request preferred ATT MTU (tells peer "I prefer 247")
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
        ESP_LOGI(TAG, "[Health] rb_fill=%.1f%% overflows=%lu", fill, rb_overflow_count);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


/* ---------- app_main ---------- */
void app_main(void) {
    esp_log_level_set("NimBLE", ESP_LOG_ERROR);

    ESP_LOGI(TAG, "Speechster H2 starting");

    state_lock = xSemaphoreCreateMutex();
    audio_rb = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!audio_rb) {
        ESP_LOGE(TAG, "ringbuffer create failed");
        return;
    }

    // ADC (legacy call) - acceptable here.
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(FSR_ADC_CHANNEL, ADC_ATTEN_DB_12);

    // I2S
    i2s_init_inmp441();

    // NimBLE
    init_nimble();

    // start tasks
    xTaskCreate(i2s_capture_task, "i2s_cap", 8*1024, NULL, 6, NULL);
    //xTaskCreate(fsr_task, "fsr", 4*1024, NULL, 5, NULL);
    xTaskCreate(ble_sender_task, "ble_send", 8*1024, NULL, 5, NULL);

    //xTaskCreate(mic_test_task, "mic_test", 4096, NULL, 5, NULL);
    xTaskCreate(telemetry_task, "telemetry", 2048, NULL, 1, NULL);


    ESP_LOGI(TAG, "Main started; advertising automatically, UART logs enabled");
}
