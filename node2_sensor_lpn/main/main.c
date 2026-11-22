#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_low_power_api.h"

#include "dht11.h"
#include "mesh_bt.h"
#include "mesh_config.h"
#include "mesh_payload.h"
#include "rgb_led.h"

#define TAG "SENSOR"

static uint8_t dev_uuid[16];
static volatile bool friendship;

static void lpn_start(void)
{
    esp_err_t err = esp_ble_mesh_lpn_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LPN gagal aktif: %s", esp_err_to_name(err));
    }
}

static void prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "Provisioning selesai, alamat 0x%04x", param->node_prov_complete.addr);
        lpn_start();
        rgb_led_set(0, 0, 32);
        break;
    case ESP_BLE_MESH_LPN_FRIENDSHIP_ESTABLISH_EVT:
        friendship = true;
        ESP_LOGI(TAG, "Friendship terbentuk, friend 0x%04x", param->lpn_friendship_establish.friend_addr);
        rgb_led_set(0, 32, 0);
        break;
    case ESP_BLE_MESH_LPN_FRIENDSHIP_TERMINATE_EVT:
        friendship = false;
        ESP_LOGW(TAG, "Friendship putus");
        rgb_led_set(0, 0, 32);
        break;
    default:
        break;
    }
}

static esp_ble_mesh_cfg_srv_t cfg_srv = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(vnd_pub, SENSOR_PAYLOAD_LEN + 3, ROLE_NODE);
static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(MESH_CID, MESH_VND_MODEL_ID, vnd_op, &vnd_pub, NULL),
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&cfg_srv),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid = MESH_CID,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
};

static void sensor_task(void *arg)
{
    uint8_t buf[SENSOR_PAYLOAD_LEN];
    sensor_reading_t r;

    /* DHT11 butuh waktu setelah power-on. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SENSOR_PERIOD_S * 1000));

        if (!esp_ble_mesh_node_is_provisioned()) {
            ESP_LOGI(TAG, "Belum provisioned, menunggu node 4");
            continue;
        }
        if (!dht11_read((gpio_num_t)CONFIG_DHT11_GPIO, &r)) {
            ESP_LOGW(TAG, "Baca DHT11 gagal, coba lagi di periode berikutnya");
            continue;
        }
        ESP_LOGI(TAG, "LPN bangun: suhu %d C, kelembapan %d %%", r.temp_c, r.hum_pct);

        if (!friendship) {
            ESP_LOGW(TAG, "Belum ada friendship, data tidak dikirim");
            continue;
        }
        sensor_encode(&r, buf);
        esp_err_t err = esp_ble_mesh_model_publish(&vnd_models[0], MESH_VND_OP_DATA,
                                                   SENSOR_PAYLOAD_LEN, buf, ROLE_NODE);
        ESP_LOGI(TAG, "Kirim ke grup 0x%04x: %s", MESH_GROUP_SENSOR, esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(gpio_pullup_en((gpio_num_t)CONFIG_DHT11_GPIO));
    ESP_ERROR_CHECK(rgb_led_init());
    ESP_ERROR_CHECK(mesh_bt_init());
    ESP_LOGI(TAG, "Bluetooth siap");
    mesh_make_uuid(dev_uuid, MESH_ROLE_SENSOR);

    ESP_ERROR_CHECK(esp_ble_mesh_register_prov_callback(prov_cb));
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));
    ESP_LOGI(TAG, "Mesh siap");

    bool provisioned = esp_ble_mesh_node_is_provisioned();
    ESP_LOGI(TAG, "Boot, sudah provisioned: %s", provisioned ? "ya" : "belum");
    if (provisioned) {
        lpn_start();
        rgb_led_set(0, 0, 32);
    } else {
        err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        ESP_LOGI(TAG, "Prov enable: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(err);
    }

    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
}
