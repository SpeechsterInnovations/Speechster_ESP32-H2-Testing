#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ESP32H2_BLE";

// UUIDs for custom service and characteristics
#define SERVICE_UUID        "0000feed-0000-1000-8000-00805f9b34fb"
#define CHAR_RX_UUID        "feed0001-0010-0000-8000-00805f9b34fb" // Web -> ESP
#define CHAR_TX_UUID        "feed0002-0010-0000-8000-00805f9b34fb" // ESP -> Web

// Handle writes from Web Bluetooth
static int on_write(uint16_t conn_handle, uint16_t attr_handle,
                    struct ble_gatt_access_ctxt *ctxt, void *arg) {
    ESP_LOGI(TAG, "Data written from Web: %.*s",
             ctxt->om->om_len, ctxt->om->om_data);
    return 0;
}

// GATT service definition
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
                                    0x00,0x10,0x00,0x00,0xed,0xfe,0x00,0x00),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID128_DECLARE(0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
                                            0x00,0x10,0x01,0x00,0x00,0xed,0xfe,0x00),
                .access_cb = on_write,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID128_DECLARE(0xfb,0x34,0x9b,0x5f,0x80,0x00,0x00,0x80,
                                            0x00,0x10,0x02,0x00,0x00,0xed,0xfe,0x00),
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0} // End of characteristics
        },
    },
    {0} // End of services
};

void ble_app_on_sync(void) {
    ble_svc_gap_device_name_set("ESP32H2_WebBLE");

    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, NULL, NULL);

    ESP_LOGI(TAG, "BLE advertising started");
}

void ble_host_task(void *param) {
    nimble_port_run();
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Initialize the NimBLE controller
    esp_nimble_hci_init();

    // Initialize NimBLE host
    nimble_port_init();

    // Callback when BLE stack is synced
    ble_hs_cfg.sync_cb = ble_app_on_sync;

    // Run BLE host task
    nimble_port_freertos_init(ble_host_task);
}
