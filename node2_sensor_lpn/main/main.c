#include "esp_log.h"
#include "esp_timer.h"
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
#define LPN_FALLBACK_DELAY_US (12 * 1000 * 1000LL)

static uint8_t dev_uuid[16];
static volatile bool friendship;
static volatile bool lpn_started;
static esp_timer_handle_t lpn_delay_timer;

static void lpn_start(void)
{
    if (lpn_started) {
        return;
    }
    lpn_started = true;
    if (lpn_delay_timer) {
        esp_timer_stop(lpn_delay_timer);
    }
    esp_err_t err = esp_ble_mesh_lpn_enable();
    if (err != ESP_OK) {
        lpn_started = false;
        ESP_LOGE(TAG, "LPN gagal aktif: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "LPN diaktifkan, mencari Friend (Node 3)");
    }
}

static void lpn_fallback_cb(void *arg)
{
    if (!lpn_started && esp_ble_mesh_node_is_provisioned()) {
        ESP_LOGW(TAG, "Timeout tunggu MODEL_PUB_SET, aktifkan LPN via fallback");
        lpn_start();
    }
}

static void cfg_srv_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                       esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        return;
    }
    if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET) {
        ESP_LOGI(TAG, "Konfigurasi publish selesai dari provisioner, mulai LPN");
        lpn_start();
    }
}

static void prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "Provisioning selesai, alamat 0x%04x (menunggu config dari Node 4)",
                 param->node_prov_complete.addr);
        rgb_led_set(0, 0, 32);
        /* Tunda LPN sampai Node 4 selesai mengirim AppKey, Bind, dan Pub Set. */
        if (lpn_delay_timer) {
            esp_timer_start_once(lpn_delay_timer, LPN_FALLBACK_DELAY_US);
        }
        break;
    case ESP_BLE_MESH_LPN_FRIENDSHIP_ESTABLISH_EVT:
        friendship = true;
        ESP_LOGI(TAG, "Friendship terbentuk, friend 0x%04x", param->lpn_friendship_establish.friend_addr);
        rgb_led_set(0, 32, 0);
        break;
    case ESP_BLE_MESH_LPN_FRIENDSHIP_TERMINATE_EVT:
        friendship = false;
        ESP_LOGW(TAG, "Friendship putus dengan friend 0x%04x", param->lpn_friendship_terminate.friend_addr);
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
    ESP_BLE_MESH_MODEL_OP(MESH_VND_OP_DATA, SENSOR_PAYLOAD_LEN_V1),
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
    uint8_t seq = 0;
    sensor_reading_t r = {0};

    /* DHT11 butuh waktu stabilisasi setelah power-on. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_SENSOR_PERIOD_S * 1000));

        if (!esp_ble_mesh_node_is_provisioned()) {
            ESP_LOGI(TAG, "Belum provisioned, menunggu Node 4");
            continue;
        }
        if (!dht11_read((gpio_num_t)CONFIG_DHT11_GPIO, &r)) {
            ESP_LOGW(TAG, "Baca DHT11 gagal, coba lagi di periode berikutnya");
            continue;
        }
        r.seq = ++seq;
        r.has_seq = true;
        ESP_LOGI(TAG, "LPN bangun [#%u]: suhu %d C, kelembapan %d %%", r.seq, r.temp_c, r.hum_pct);

        if (!friendship) {
            ESP_LOGW(TAG, "Belum ada friendship, data #%u tidak dikirim", r.seq);
            continue;
        }
        sensor_encode(&r, buf);
        esp_err_t err = esp_ble_mesh_model_publish(&vnd_models[0], MESH_VND_OP_DATA,
                                                   SENSOR_PAYLOAD_LEN, buf, ROLE_NODE);
        ESP_LOGI(TAG, "Kirim #%u ke grup 0x%04x: %s", r.seq, MESH_GROUP_SENSOR, esp_err_to_name(err));
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

    gpio_reset_pin((gpio_num_t)CONFIG_DHT11_GPIO);
    ESP_ERROR_CHECK(gpio_pullup_en((gpio_num_t)CONFIG_DHT11_GPIO));
    ESP_ERROR_CHECK(rgb_led_init());
    ESP_ERROR_CHECK(mesh_bt_init());
    ESP_LOGI(TAG, "Bluetooth siap (TX +20 dBm)");
    mesh_make_uuid(dev_uuid, MESH_ROLE_SENSOR);

    esp_timer_create_args_t targs = {
        .callback = lpn_fallback_cb,
        .name = "lpn_delay",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &lpn_delay_timer));

    ESP_ERROR_CHECK(esp_ble_mesh_register_prov_callback(prov_cb));
    ESP_ERROR_CHECK(esp_ble_mesh_register_config_server_callback(cfg_srv_cb));
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

    /* Pin ke Core 1 agar pembacaan DHT11 tidak pernah memblokir BLE di Core 0. */
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 5, NULL, 1);
}
