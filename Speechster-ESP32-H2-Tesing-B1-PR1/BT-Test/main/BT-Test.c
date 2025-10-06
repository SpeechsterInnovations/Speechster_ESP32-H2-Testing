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
#define I2S_BCK_IO           4
#define I2S_WS_IO            5
#define I2S_DATA_IN_IO       10

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

/* ---------- I2S (INMP441) ---------- */
static void i2s_init_inmp441(void) {
    i2s_config_t cfg = {
        .mode = I2S_MODE_SLAVE | I2S_MODE_RX,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = 16,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB, // modern constant, avoids deprecated enum
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .intr_alloc_flags = 0,
        .dma_buf_count = 6,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = false
    };
    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DATA_IN_IO
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT_NUM, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT_NUM, &pins));
    ESP_ERROR_CHECK(i2s_set_clk(I2S_PORT_NUM, AUDIO_SAMPLE_RATE, 16, I2S_CHANNEL_MONO));
    ESP_LOGI(TAG, "I2S init done (BCK=%d, WS=%d, SD=%d, SR=%d)",
             I2S_BCK_IO, I2S_WS_IO, I2S_DATA_IN_IO, AUDIO_SAMPLE_RATE);
}

static void i2s_capture_task(void *arg) {
    ESP_LOGI(TAG, "i2s_capture_task start");
    uint8_t *buf = (uint8_t*) malloc(AUDIO_READ_BYTES);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for i2s buffer");
        vTaskDelete(NULL);
        return;
    }

    adpcm_state_t adpcm_state;
    adpcm_init_state(&adpcm_state);

    while (1) {
        if (!mic_enabled) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t read = 0;
        esp_err_t r = i2s_read(I2S_PORT_NUM, buf, AUDIO_READ_BYTES, &read, pdMS_TO_TICKS(200));
        if (r != ESP_OK || read == 0) {
            continue;
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

/* ---------- GATT: control (write JSON) ---------- */
static int gatt_control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0) return 0;
    char *json = (char*) malloc(len + 1);
    if (!json) return 0;
    os_mbuf_copydata(ctxt->om, 0, len, json);
    json[len] = '\0';
    ESP_LOGI(TAG, "Control JSON: %s", json);

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON in control");
        free(json);
        return 0;
    }

    xSemaphoreTake(state_lock, pdMS_TO_TICKS(200));

    cJSON *jmic = cJSON_GetObjectItem(root, "mic");
    if (jmic && cJSON_IsBool(jmic)) {
        mic_enabled = cJSON_IsTrue(jmic);
        ESP_LOGI(TAG, "mic_enabled=%d", mic_enabled);
    }

    cJSON *jfsr = cJSON_GetObjectItem(root, "fsr");
    if (jfsr && cJSON_IsBool(jfsr)) {
        fsr_enabled = cJSON_IsTrue(jfsr);
        ESP_LOGI(TAG, "fsr_enabled=%d", fsr_enabled);
    }

    cJSON *jenc = cJSON_GetObjectItem(root, "encoder");
    if (jenc && cJSON_IsString(jenc)) {
        if (strcasecmp(jenc->valuestring, "pcm") == 0) {
            current_encoder = ENC_PCM; ESP_LOGI(TAG,"encoder -> PCM");
        } else if (strcasecmp(jenc->valuestring, "adpcm") == 0) {
            current_encoder = ENC_ADPCM; adpcm_reset_state(); ESP_LOGI(TAG,"encoder -> ADPCM");
        }
    }

    cJSON *jint = cJSON_GetObjectItem(root, "fsr_interval_ms");
    if (jint && cJSON_IsNumber(jint)) {
        uint32_t v = (uint32_t) jint->valuedouble;
        if (v >= 10 && v <= 5000) {
            fsr_interval_ms = v;
            ESP_LOGI(TAG, "fsr_interval_ms=%u", fsr_interval_ms);
        }
    }

    xSemaphoreGive(state_lock);
    cJSON_Delete(root);
    free(json);
    return 0;
}

static int gatt_stream_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // Notify-only characteristic: reads not supported
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
                .access_cb = gatt_stream_access_cb,
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
    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
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

/* ---------- BLE sender: pop frames and notify ---------- */
static void ble_sender_task(void *arg) {
    ESP_LOGI(TAG, "ble_sender_task start");
    while (1) {
        size_t item_size;
        uint8_t *item = (uint8_t*) xRingbufferReceive(audio_rb, &item_size, pdMS_TO_TICKS(500));
        if (!item) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        if (g_conn_handle == 0 || g_stream_attr_handle == 0) {
            vPortFree(item);
            continue;
        }

        struct os_mbuf *om = ble_hs_mbuf_from_flat(item, item_size);
        if (!om) { ESP_LOGW(TAG, "mbuf alloc failed"); vPortFree(item); continue; }

        int rc = ble_gatts_notify_custom(g_conn_handle, g_stream_attr_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gatts_notify_custom rc=%d", rc);
        }
        vPortFree(item);
    }
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

    // ADC (legacy call) - acceptable here. If you want full migration, I will provide.
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(FSR_ADC_CHANNEL, ADC_ATTEN_DB_11);

    // I2S
    i2s_init_inmp441();

    // NimBLE
    init_nimble();

    // start tasks
    xTaskCreate(i2s_capture_task, "i2s_cap", 8*1024, NULL, 6, NULL);
    xTaskCreate(fsr_task, "fsr", 4*1024, NULL, 5, NULL);
    xTaskCreate(ble_sender_task, "ble_send", 8*1024, NULL, 5, NULL);

    ESP_LOGI(TAG, "Main started; advertising automatically, UART logs enabled");
}
