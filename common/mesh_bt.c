#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "mesh_bt.h"
#include "mesh_config.h"

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
