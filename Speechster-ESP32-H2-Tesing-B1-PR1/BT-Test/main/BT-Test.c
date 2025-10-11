// main.c
// ESP-IDF v5.5.1 (dirty) - NimBLE GATT server with JSON control + stream notify
// INMP441 I2S pins: SD=GPIO10, SCK=GPIO4, WS=GPIO5 (L/R tied to GND -> left channel)
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
#include "driver/i2s.h"
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
#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_FRAME_MS       20
#define AUDIO_FRAME_SAMPLES  ((AUDIO_SAMPLE_RATE/1000) * AUDIO_FRAME_MS) // 320
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_READ_BYTES     (AUDIO_FRAME_SAMPLES * AUDIO_BYTES_PER_SAMPLE)
#define AUDIO_RINGBUF_SIZE   (64 * 1024)

#define I2S_PORT_NUM         I2S_NUM_0
#define I2S_BCK_IO           2
#define I2S_WS_IO            14
#define I2S_DATA_IN_IO       10
#define I2S_NUM         I2S_NUM_0
#define SAMPLE_BUF_LEN  320  // Number of samples per read


#define FSR_ADC_CHANNEL      ADC_CHANNEL_1
static uint32_t fsr_interval_ms = 50; // default 50ms, can be changed via JSON

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
    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 outputs 24-bit in 32-bit slots
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
        .dma_buf_count = 6,
        .dma_buf_len = 256,
        .use_apll = false,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    };

    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DATA_IN_IO
    };
    
    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT_NUM, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT_NUM, &pins));
    ESP_ERROR_CHECK(i2s_set_clk(I2S_PORT_NUM, 16000, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO));
    ESP_LOGI(TAG, "INMP441 I2S configured: BCK=%d WS=%d SD=%d", I2S_BCK_IO, I2S_WS_IO, I2S_DATA_IN_IO);
}


void mic_test_task(void *param) {
    mic_enabled = true;

    int32_t i2s_buf[SAMPLE_BUF_LEN];
    ESP_LOGI(TAG, "Starting mic test task...");

    while (1) {
        size_t bytes_read;
        if (i2s_read(I2S_NUM, i2s_buf, SAMPLE_BUF_LEN*sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed");
            continue;
        }

        int16_t s16;
        int16_t peak = 0;
        int64_t sum_sq = 0;

        for (int i = 0; i < SAMPLE_BUF_LEN; i++) {
            s16 = (int16_t)(i2s_buf[i] >> 14); // 24-bit → 16-bit
            if (abs(s16) > peak) peak = abs(s16);
            sum_sq += (int64_t)s16 * s16;
        }

        double rms = sqrt((double)sum_sq / SAMPLE_BUF_LEN);
        double dB  = (rms > 0) ? 20.0 * log10(rms / 32768.0) : -180.0;

        ESP_LOGI(TAG, "[MIC_DATA] %d samples, peak=%d, dB=%.1f", SAMPLE_BUF_LEN, peak, dB);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}




static void i2s_capture_task(void *arg) {
    ESP_LOGI(TAG, "i2s_capture_task start");
    ESP_LOGI(TAG, "[MIC] Capture task initialized, waiting for mic_enabled=true...");
    uint8_t *buf = (uint8_t*) malloc(AUDIO_READ_BYTES);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for i2s buffer");
        vTaskDelete(NULL);
        return;
    }

    adpcm_state_t adpcm_state;
    adpcm_init_state(&adpcm_state);

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
        // Read 32-bit mic samples, but we only need lower 16 bits for BLE streaming
        size_t bytes32 = AUDIO_FRAME_SAMPLES * sizeof(int32_t);
        int32_t *i2s32 = (int32_t*) malloc(bytes32);
        if (!i2s32) { ESP_LOGE(TAG, "malloc fail i2s32"); vTaskDelete(NULL); }

        size_t read32 = 0;
        esp_err_t r = i2s_read(I2S_PORT_NUM, i2s32, bytes32, &read32, pdMS_TO_TICKS(200));
        if (r != ESP_OK || read32 == 0) {
            free(i2s32);
            continue;
        }
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "[MIC_ERR] i2s_read failed: %s", esp_err_to_name(r));
        } else if (read32 == 0) {
            ESP_LOGW(TAG, "[MIC_WARN] i2s_read returned 0 bytes");
        }

        // Convert 32-bit → 16-bit
        size_t samples = read32 / sizeof(int32_t);
        for (size_t i = 0; i < samples; i++) {
            ((int16_t*)buf)[i] = (int16_t)(i2s32[i] >> 14); // keep significant bits
        }
        free(i2s32);
        size_t read = samples * sizeof(int16_t);
        if (r != ESP_OK || read == 0) {
            continue;
        }

        static uint32_t last_log_ms = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now_ms - last_log_ms > 1000) {  // log once per second
            int16_t first = ((int16_t*)buf)[0];
            int16_t peak = 0;
            for (size_t i = 0; i < samples; i++) {
                int16_t v = ((int16_t*)buf)[i];
                if (abs(v) > peak) peak = abs(v);
            }

        // --- RMS and dB calculation ---
        double sum_sq = 0;
        for (size_t i = 0; i < samples; i++) {
            int16_t sample = ((int16_t*)buf)[i];
            sum_sq += (double)sample * sample;
        }
        double rms = sqrt(sum_sq / samples);
        double db = 20.0 * log10(rms / 32768.0 + 1e-9); // avoid log(0)
        // --------------------------------

            ESP_LOGI(TAG, "[MIC_DATA] %u samples, peak=%d, first=%d, dB=%.1f", (unsigned)samples, peak, first, db);
            last_log_ms = now_ms;
        }

        encoder_t enc = current_encoder;
        if (enc != ENC_PCM && enc != ENC_ADPCM) enc = default_encoder;

        if (enc == ENC_PCM) {
            size_t frame_sz = 8 + read;
            uint8_t *frame = (uint8_t*) pvPortMalloc(frame_sz);
            if (!frame) { ESP_LOGW(TAG, "no mem for pcm frame"); continue; }
            uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint16_t seq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
            pack_header(frame, 0x01, 0x00, seq, ts);
            memcpy(frame + 8, buf, read);
            if (xRingbufferSend(audio_rb, frame, frame_sz, pdMS_TO_TICKS(50)) != pdTRUE) {
                vPortFree(frame);
            }
        } else { // ADPCM
            const int16_t *pcm = (const int16_t*) buf;
            size_t samples = read / 2;
            size_t max_out = samples/2 + 16;
            uint8_t *outb = (uint8_t*) pvPortMalloc(max_out);
            if (!outb) { ESP_LOGW(TAG, "no mem adpcm out"); continue; }
            size_t out_bytes = adpcm_encode(&adpcm_state, pcm, samples, outb);
            size_t frame_sz = 8 + out_bytes;
            uint8_t *frame = (uint8_t*) pvPortMalloc(frame_sz);
            if (!frame) { vPortFree(outb); continue; }
            uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint16_t seq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
            pack_header(frame, 0x01, 0x01, seq, ts);
            memcpy(frame + 8, outb, out_bytes);
            vPortFree(outb);
            if (xRingbufferSend(audio_rb, frame, frame_sz, pdMS_TO_TICKS(50)) != pdTRUE) {
                vPortFree(frame);
            }
        }
    }
    free(buf);
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
            adpcm_reset_state();
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

/* ---------- BLE sender: hybrid fake + real streaming ---------- */

static void ble_sender_task(void *arg) {
    ESP_LOGI(TAG, "ble_sender_task start");

    while (1) {
        /* --- 1) Fallback: send fake frames when fake_mode is ON --- */
        if (fake_mode && g_stream_attr_handle > 0) {
            uint8_t fake_payload[4] = { 0x11, 0x22, 0x33, 0x44 };
            uint8_t fake_frame[8 + sizeof(fake_payload)];
            uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint16_t seq = __atomic_fetch_add(&seq_counter, 1, __ATOMIC_SEQ_CST);
            pack_header(fake_frame, 0x01, 0x00, seq, ts);
            memcpy(fake_frame + 8, fake_payload, sizeof(fake_payload));

            struct os_mbuf *om = ble_hs_mbuf_from_flat(fake_frame, sizeof(fake_frame));
            if (om) {
                int rc = ble_gatts_notify_custom(g_conn_handle, g_stream_attr_handle, om);
                if (rc == 0) {
                    ESP_LOGI(TAG, "[FAKE] Sent %u bytes (seq=%u)", (unsigned)sizeof(fake_frame), seq);
                } else {
                    ESP_LOGW(TAG, "[FAKE] notify rc=%d", rc);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* --- 2) Real streaming: pull from ringbuffer and notify --- */
        size_t item_size;
        uint8_t *item = (uint8_t*) xRingbufferReceive(audio_rb, &item_size, pdMS_TO_TICKS(500));

        if (!item) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (g_conn_handle == 0 || g_stream_attr_handle == 0) {
            vPortFree(item);
            continue;
        }

        uint8_t type = item[0];
        uint16_t seq = item[2] | (item[3] << 8);
        uint32_t ts  = item[4] | (item[5] << 8) | (item[6] << 16) | (item[7] << 24);
        LOG_STREAM("ts=%u seq=%u type=0x%02X len=%u", ts, seq, type, (unsigned)item_size);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(item, item_size);
        if (!om) {
            ESP_LOGW(TAG, "mbuf alloc failed");
            vPortFree(item);
            continue;
        }

        int rc = ble_gatts_notify_custom(g_conn_handle, g_stream_attr_handle, om);
        if (rc == 0) {
            ESP_LOGI(TAG, "[BLE_SEND] type=0x%02X seq=%u ts=%u size=%u bytes",
                    type, seq, ts, (unsigned)item_size);
        } else {
            ESP_LOGW(TAG, "[BLE_SEND_FAIL] rc=%d seq=%u size=%u", rc, seq, (unsigned)item_size);
        }
        vPortFree(item);
    }
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
                .access_cb = gatt_control_access_cb,
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

            // As a backup/request: call ble_att_set_preferred_mtu again for this connection
            int rc = ble_att_set_preferred_mtu(247);
            ESP_LOGI(TAG, "ble_att_set_preferred_mtu on connect rc=%d", rc);

            /* Request conn params: 7.5 ms interval */
            struct ble_gap_upd_params params;
            params.itvl_min = 6;   // 6 * 1.25 ms = 7.5 ms
            params.itvl_max = 6;   // keep fixed
            params.latency  = 0;
            params.supervision_timeout = 400; // 400 * 10ms = 4s
            int upd_rc = ble_gap_update_params(g_conn_handle, &params);
            ESP_LOGI(TAG, "ble_gap_update_params rc=%d", upd_rc);

        } else {
            ESP_LOGW(TAG, "Connect failed; status=%d", event->connect.status);
            ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
        g_conn_handle = 0;
        g_stream_attr_handle = 0;
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe: conn=%d attr_handle=%d cur_notify=%d", event->subscribe.conn_handle, event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.cur_notify) {
            g_stream_attr_handle = event->subscribe.attr_handle;
            ESP_LOGI(TAG, "Cached stream attr handle: %d", g_stream_attr_handle);
        } else {
            if (g_stream_attr_handle == event->subscribe.attr_handle) g_stream_attr_handle = 0;
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU exchange event: conn=%d mtu=%u", event->mtu.conn_handle, event->mtu.value);
        // also query via API:
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
    int rc = ble_att_set_preferred_mtu(247);
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
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;
    nimble_port_freertos_init(nimble_host_task);
}

/* ---------- app_main ---------- */
void app_main(void) {
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "Speechster H2 starting");

    state_lock = xSemaphoreCreateMutex();
    audio_rb = xRingbufferCreate(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!audio_rb) {
        ESP_LOGE(TAG, "ringbuffer create failed");
        return;
    }

    // ADC (legacy call) - acceptable here.
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(FSR_ADC_CHANNEL, ADC_ATTEN_DB_11);

    // I2S
    i2s_init_inmp441();

    // NimBLE
    // init_nimble();

    // start tasks
    xTaskCreate(i2s_capture_task, "i2s_cap", 8*1024, NULL, 6, NULL);
    //xTaskCreate(fsr_task, "fsr", 4*1024, NULL, 5, NULL);
    // xTaskCreate(ble_sender_task, "ble_send", 8*1024, NULL, 5, NULL);

    xTaskCreate(mic_test_task, "mic_test", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Main started; advertising automatically, UART logs enabled");
}
