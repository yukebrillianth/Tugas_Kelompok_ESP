#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_log.h"

#include "mesh_bt.h"
#include "mesh_config.h"

#define TAG "MESH_BT"

/* ESP32-C3 Super Mini punya antena keramik kecil yang sering gagal TX di +20 dBm,
 * jadi dibatasi di +9 dBm; ESP32-S3 N16R8 dimaksimalkan ke +20 dBm. */
#if CONFIG_IDF_TARGET_ESP32C3
#define MESH_BLE_TX_PWR     ESP_PWR_LVL_P9
#define MESH_BLE_TX_PWR_DBM 9
#else
#define MESH_BLE_TX_PWR     ESP_PWR_LVL_P20
#define MESH_BLE_TX_PWR_DBM 20
#endif

esp_err_t mesh_bt_init(void)
{
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        return err;
    }

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        return err;
    }

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, MESH_BLE_TX_PWR);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, MESH_BLE_TX_PWR);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, MESH_BLE_TX_PWR);
    ESP_LOGI(TAG, "BLE TX power diset ke +%d dBm", MESH_BLE_TX_PWR_DBM);

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        return err;
    }

    return esp_bluedroid_enable();
}

void mesh_make_uuid(uint8_t uuid[16], uint8_t role)
{
    memset(uuid, 0, 16);
    uuid[0] = MESH_UUID_TAG;
    uuid[1] = role;
    memcpy(uuid + 2, esp_bt_dev_get_address(), 6);
}
