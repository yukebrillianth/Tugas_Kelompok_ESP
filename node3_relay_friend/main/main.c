#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"

#include "mesh_bt.h"
#include "mesh_config.h"
#include "rgb_led.h"

#define TAG "RELAY"

static uint8_t dev_uuid[16];

static void prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "Provisioning selesai, alamat 0x%04x", param->node_prov_complete.addr);
        rgb_led_set(0, 32, 0);
        break;
    case ESP_BLE_MESH_FRIEND_FRIENDSHIP_ESTABLISH_EVT:
        ESP_LOGI(TAG, "Friendship dengan LPN 0x%04x terbentuk", param->frnd_friendship_establish.lpn_addr);
        break;
    case ESP_BLE_MESH_FRIEND_FRIENDSHIP_TERMINATE_EVT:
        ESP_LOGW(TAG, "Friendship dengan LPN putus");
        break;
    default:
        break;
    }
}

static esp_ble_mesh_cfg_srv_t cfg_srv = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
    .default_ttl = 7,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&cfg_srv),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = MESH_CID,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
};

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(rgb_led_init());
    ESP_ERROR_CHECK(mesh_bt_init());
    mesh_make_uuid(dev_uuid, MESH_ROLE_RELAY);

    ESP_ERROR_CHECK(esp_ble_mesh_register_prov_callback(prov_cb));
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));

    if (!esp_ble_mesh_node_is_provisioned()) {
        ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
    } else {
        rgb_led_set(0, 32, 0);
    }

    ESP_LOGI(TAG, "Relay dan Friend aktif, selalu menyala");
}
