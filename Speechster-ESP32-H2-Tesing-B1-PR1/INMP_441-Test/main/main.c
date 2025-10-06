/* main.c - NimBLE-based BLE RMS notifier for ESP32-H2 (ESP-IDF)
 *
 * Advertises as "SpeechsterH2"
 * Custom 128-bit service + characteristic (notify)
 * Notifies RMS as uint16_t (rms * 100)
 *
 * NOTE: Enable NimBLE in menuconfig (Component config -> Bluetooth -> NimBLE)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"

/* NimBLE / host headers */
#include "esp_nimble_hci.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "NIMBLE_RMS";

/* I2S / INMP441 settings */
#define SAMPLE_RATE        16000
#define I2S_NUM            (I2S_NUM_0)

#define I2S_BCLK_PIN       GPIO_NUM_4
#define I2S_WS_PIN         GPIO_NUM_5
#define I2S_SD_PIN         GPIO_NUM_10

#define I2S_READ_SAMPLES   256
#define RMS_WINDOW_SAMPLES (SAMPLE_RATE / 10) // 100ms

/* queue for samples from producer -> RMS calc */
static QueueHandle_t s_sample_queue = NULL;

/* 128-bit UUIDs (little-endian) */
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x78,0x56,0x34,0x12,0x34,0x12,0x78,0x56,0x12,0x34,0x78,0x56,0x12,0x34,0x12,0x34);

static const ble_uuid128_t char_uuid = BLE_UUID128_INIT(
    0x88,0x66,0x44,0x22,0x34,0x12,0x78,0x56,0x12,0x34,0x78,0x56,0x12,0x34,0x12,0x35);

/* handles */
static int g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static int g_char_attr_handle = 0;

/* Forward declarations */
static int gatt_access_rd(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

/* GATT service definition (NimBLE) */
static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t*)&service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = (ble_uuid_t*)&char_uuid.u,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .access_cb = gatt_access_rd,
                .val_handle = &g_char_attr_handle,
            },
            { 0 } /* terminator for characteristic array */
        },
    },
    { 0 } /* terminator for service array */
};

/* --- I2S initialization (std) --- */
static i2s_chan_handle_t s_rx_handle = NULL;

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = 64;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
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
    return err;
}

/* --- Producer --- */
static void i2s_producer_task(void *arg)
{
    const size_t read_bytes = I2S_READ_SAMPLES * sizeof(int32_t);
    int32_t *i2s_buf = (int32_t *)heap_caps_malloc(read_bytes, MALLOC_CAP_DEFAULT);
    if (!i2s_buf) {
        ESP_LOGE(TAG, "Failed to allocate i2s buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(s_rx_handle, i2s_buf, read_bytes, &bytes_read, pdMS_TO_TICKS(200));
        if (r == ESP_OK && bytes_read > 0) {
            size_t frames = bytes_read / sizeof(int32_t);
            /* debug: log first sample occasionally to ensure we're getting data */
            if (frames > 0) {
                int32_t s32 = i2s_buf[0];
                ESP_LOGD(TAG, "i2s read frames=%u first_sample=0x%08x", (unsigned)frames, (unsigned)s32);
            }
            for (size_t i = 0; i < frames; ++i) {
                int32_t s32 = i2s_buf[i];
                int16_t s16 = (int16_t)(s32 >> 14); // crude 24->16 conversion
                xQueueSend(s_sample_queue, &s16, 0);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    heap_caps_free(i2s_buf);
    vTaskDelete(NULL);
}

/* --- RMS + notify task --- */
static void rms_and_notify_task(void *arg)
{
    const size_t window = RMS_WINDOW_SAMPLES;
    int32_t *circ = (int32_t *)heap_caps_malloc(sizeof(int32_t) * window, MALLOC_CAP_DEFAULT);
    if (!circ) {
        ESP_LOGE(TAG, "Failed to alloc circ buffer");
        vTaskDelete(NULL);
        return;
    }
    size_t wp = 0;
    size_t count = 0;
    double sum_sq = 0.0;

    while (1) {
        int16_t sample;
        if (xQueueReceive(s_sample_queue, &sample, pdMS_TO_TICKS(200)) == pdTRUE) {
            int32_t prev = (count < window) ? 0 : circ[wp];
            int32_t val = sample;
            circ[wp] = val;
            wp = (wp + 1) % window;
            if (count < window) {
                count++;
                sum_sq += (double)val * (double)val;
            } else {
                sum_sq -= (double)prev * (double)prev;
                sum_sq += (double)val * (double)val;
            }
        }

        if (count > 0) {
            double rms = sqrt(sum_sq / (double)count) / 32768.0; // normalized 0..1
            ESP_LOGI("INMP441_RMS", "Audio RMS = %.2f", (float)rms);

            if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE && g_char_attr_handle != 0) {
                uint16_t scaled = (uint16_t)(rms * 100.0f);
                uint8_t payload[2];
                payload[0] = (uint8_t)(scaled & 0xff);
                payload[1] = (uint8_t)((scaled >> 8) & 0xff);

                /* build an os_mbuf from the flat payload and notify */
                struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
                if (om) {
                    int rc = ble_gatts_notify_custom(g_conn_handle, g_char_attr_handle, om);
                    if (rc) {
                        ESP_LOGW(TAG, "ble_gatts_notify_custom rc=%d", rc);
                        os_mbuf_free_chain(om);
                    } else {
                        ESP_LOGD(TAG, "notify sent");
                    }
                } else {
                    ESP_LOGW(TAG, "failed to allocate os_mbuf for notify");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    heap_caps_free(circ);
    vTaskDelete(NULL);
}

/* GATT read access callback - return a two-byte current value when read */
static int gatt_access_rd(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Return 0x0000 for read (this is just a simple read handler). */
    uint8_t zero[2] = {0, 0};
    int rc = os_mbuf_append(ctxt->om, zero, sizeof(zero));
    return rc == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
}

/* GAP callbacks (connect/disconnect) */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Client connected; conn_handle=%d", event->connect.conn_handle);
                g_conn_handle = event->connect.conn_handle;
            } else {
                ESP_LOGW(TAG, "Connection failed; status=%d", event->connect.status);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Client disconnected; reason=%d", event->disconnect.reason);
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            /* re-start advertising */
            {
                struct ble_gap_adv_params advp;
                memset(&advp, 0, sizeof(advp));
                advp.conn_mode = BLE_GAP_CONN_MODE_UND;
                advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
                ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &advp, gap_event_cb, NULL);
            }
            break;
        default:
            break;
    }
    return 0;
}

/* Called when NimBLE host is up */
static void ble_app_on_sync(void)
{
    int rc;
    uint8_t own_addr_type;

    /* Configure address */
    ble_hs_id_infer_auto(0, &own_addr_type);

    /* Add GATT services */
    rc = ble_gatts_count_cfg(gatt_services);
    if (rc) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(gatt_services);
    if (rc) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return;
    }
    /* Start GATT services (this ensures handles are populated) */
    ble_gatts_start();

    /* Setup advertising fields (include name) */
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    const char *name = "SpeechsterH2";
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &advp, gap_event_cb, NULL);
    if (rc) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising started (SpeechsterH2)");
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE reset; reason=%d", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run(); /* Blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void ble_stack_init(void)
{
    nimble_port_init();

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;

    nimble_port_freertos_init(ble_host_task);
}

/* --- App entrypoint --- */
void app_main(void)
{
    esp_err_t ret;

    /* init nvs for NimBLE */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing I2S...");
    if (i2s_init() != ESP_OK) {
        ESP_LOGE(TAG, "i2s init failed");
        return;
    }

    s_sample_queue = xQueueCreate(4096, sizeof(int16_t));
    if (!s_sample_queue) {
        ESP_LOGE(TAG, "Failed to create sample queue");
        return;
    }

    /* initialize NimBLE stack */
    ble_stack_init();

    /* start tasks */
    xTaskCreatePinnedToCore(i2s_producer_task, "i2s_prod", 4096, NULL, tskIDLE_PRIORITY + 3, NULL, 0);
    xTaskCreatePinnedToCore(rms_and_notify_task, "rms_notify", 4096, NULL, tskIDLE_PRIORITY + 2, NULL, 0);

    ESP_LOGI(TAG, "NimBLE RMS server started");
}
