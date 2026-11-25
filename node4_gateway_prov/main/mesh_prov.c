#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_proxy_api.h"

#include "mesh_bt.h"
#include "mesh_config.h"
#include "mesh_prov.h"

#define TAG                 "PROV"
#define OWN_ADDR            0x0001
#define MSG_TTL             5
#define CFG_RETRY           3
#define PUMP_MAX_RETRY      3
#define PUMP_RETRY_DELAY_US (600 * 1000LL)

typedef struct {
    uint8_t uuid[16];
    uint16_t addr;
    uint8_t role;
    uint8_t step;
    uint8_t retry;
    bool used;
} node_t;

/* Disimpan di NVS supaya node tidak perlu di-provision ulang setelah reboot. */
typedef struct {
    uint8_t app_key[16];
    node_t nodes[MESH_MAX_NODES];
} store_t;

enum { ACT_APPKEY, ACT_BIND_ONOFF, ACT_BIND_VND, ACT_PUB_VND, ACT_END };

static const int plan_pump[]   = { ACT_APPKEY, ACT_BIND_ONOFF, ACT_END };
static const int plan_sensor[] = { ACT_APPKEY, ACT_BIND_VND, ACT_PUB_VND, ACT_END };
static const int plan_relay[]  = { ACT_END };

static store_t store;
static uint8_t dev_uuid[16];
static uint8_t tid;
static esp_ble_mesh_client_t cfg_cli;
static esp_ble_mesh_client_t onoff_cli;

static mesh_sensor_cb_t sensor_cb;
static mesh_pump_cb_t pump_cb;
static mesh_log_cb_t log_cb;
static mesh_status_cb_t status_cb;

static bool proxy_enabled;
static bool proxy_connected;

/* Desired-state pump controller (thread-safe antara MQTT task dan BTC task). */
static SemaphoreHandle_t pump_mux;
static esp_timer_handle_t pump_retry_timer;
static bool pump_has_target;
static bool pump_target;
static bool pump_in_flight;
static bool pump_in_flight_val;
static uint8_t pump_retry;

/* Deduplikasi paket sensor anti-burst. */
static uint16_t last_sensor_addr;
static uint8_t last_sensor_seq;
static bool last_sensor_has_seq;
static int64_t last_sensor_us;

static void emit_log(const char *level, const char *src, const char *msg)
{
    if (log_cb) {
        log_cb(level, src, msg);
    }
}

static void emit_status(void)
{
    if (status_cb) {
        status_cb();
    }
}

static esp_ble_mesh_cfg_srv_t cfg_srv = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(MESH_VND_OP_DATA, SENSOR_PAYLOAD_LEN_V1),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&cfg_srv),
    ESP_BLE_MESH_MODEL_CFG_CLI(&cfg_cli),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(NULL, &onoff_cli),
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(MESH_CID, MESH_VND_MODEL_ID, vnd_op, NULL, NULL),
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
    .prov_uuid = dev_uuid,
    .prov_unicast_addr = OWN_ADDR,
    .prov_start_address = 0x0005,
};

static void store_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("mesh", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "store", &store, sizeof(store));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Simpan data mesh gagal: %s", esp_err_to_name(err));
    }
}

static void store_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(store);
    esp_err_t err = nvs_open("mesh", NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, "store", &store, &len);
        nvs_close(h);
    }
    if (err == ESP_OK && len == sizeof(store)) {
        return;
    }

    memset(&store, 0, sizeof(store));
    esp_fill_random(store.app_key, sizeof(store.app_key));
    store_save();
}

static node_t *node_by_addr(uint16_t addr)
{
    for (int i = 0; i < MESH_MAX_NODES; i++) {
        if (store.nodes[i].used && store.nodes[i].addr == addr) {
            return &store.nodes[i];
        }
    }
    return NULL;
}

static node_t *node_by_role(uint8_t role)
{
    for (int i = 0; i < MESH_MAX_NODES; i++) {
        if (store.nodes[i].used && store.nodes[i].role == role) {
            return &store.nodes[i];
        }
    }
    return NULL;
}

static node_t *node_add(const uint8_t uuid[16], uint16_t addr)
{
    node_t *n = NULL;
    for (int i = 0; i < MESH_MAX_NODES && !n; i++) {
        if (store.nodes[i].used && memcmp(store.nodes[i].uuid, uuid, 16) == 0) {
            n = &store.nodes[i];
        }
    }
    for (int i = 0; i < MESH_MAX_NODES && !n; i++) {
        if (!store.nodes[i].used) {
            n = &store.nodes[i];
        }
    }
    if (!n) {
        return NULL;
    }

    memset(n, 0, sizeof(*n));
    memcpy(n->uuid, uuid, 16);
    n->addr = addr;
    n->role = uuid[1];
    n->used = true;
    return n;
}

static int cfg_action(const node_t *n)
{
    const int *plan = plan_relay;
    if (n->role == MESH_ROLE_PUMP) {
        plan = plan_pump;
    } else if (n->role == MESH_ROLE_SENSOR) {
        plan = plan_sensor;
    }
    return plan[n->step];
}

static uint32_t action_opcode(int act)
{
    switch (act) {
    case ACT_APPKEY:
        return ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD;
    case ACT_PUB_VND:
        return ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET;
    default:
        return ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND;
    }
}

static void cfg_send(node_t *n)
{
    int act = cfg_action(n);
    if (act == ACT_END) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Node 0x%04x (role 0x%02x) selesai dikonfigurasi", n->addr, n->role);
        ESP_LOGI(TAG, "%s", msg);
        store_save();
        emit_log("info", "PROV", msg);
        emit_status();
        return;
    }

    esp_ble_mesh_client_common_param_t c = {0};
    esp_ble_mesh_cfg_client_set_state_t s = {0};
    c.model = cfg_cli.model;
    c.opcode = action_opcode(act);
    c.ctx.net_idx = MESH_NET_IDX;
    c.ctx.app_idx = MESH_APP_IDX;
    c.ctx.addr = n->addr;
    c.ctx.send_ttl = MSG_TTL;

    switch (act) {
    case ACT_APPKEY:
        s.app_key_add.net_idx = MESH_NET_IDX;
        s.app_key_add.app_idx = MESH_APP_IDX;
        memcpy(s.app_key_add.app_key, store.app_key, 16);
        break;
    case ACT_BIND_ONOFF:
        s.model_app_bind.element_addr = n->addr;
        s.model_app_bind.model_app_idx = MESH_APP_IDX;
        s.model_app_bind.model_id = ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV;
        s.model_app_bind.company_id = ESP_BLE_MESH_CID_NVAL;
        break;
    case ACT_BIND_VND:
        s.model_app_bind.element_addr = n->addr;
        s.model_app_bind.model_app_idx = MESH_APP_IDX;
        s.model_app_bind.model_id = MESH_VND_MODEL_ID;
        s.model_app_bind.company_id = MESH_CID;
        break;
    case ACT_PUB_VND:
        s.model_pub_set.element_addr = n->addr;
        s.model_pub_set.publish_addr = MESH_GROUP_SENSOR;
        s.model_pub_set.publish_app_idx = MESH_APP_IDX;
        s.model_pub_set.publish_ttl = 7;
        s.model_pub_set.publish_period = 0;
        /* Tanpa publish_retransmit berlebih: andalkan net_transmit agar tidak burst. */
        s.model_pub_set.publish_retransmit = 0;
        s.model_pub_set.model_id = MESH_VND_MODEL_ID;
        s.model_pub_set.company_id = MESH_CID;
        break;
    default:
        break;
    }

    esp_err_t err = esp_ble_mesh_config_client_set_state(&c, &s);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Kirim config ke 0x%04x gagal: %s", n->addr, esp_err_to_name(err));
    }
}

static void cfg_cb(esp_ble_mesh_cfg_client_cb_event_t event, esp_ble_mesh_cfg_client_cb_param_t *param)
{
    node_t *n = node_by_addr(param->params->ctx.addr);
    if (!n || cfg_action(n) == ACT_END || param->params->opcode != action_opcode(cfg_action(n))) {
        return;
    }

    if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT && param->error_code == ESP_OK) {
        n->step++;
        n->retry = 0;
        cfg_send(n);
        return;
    }
    if (event != ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT && event != ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT) {
        return;
    }

    if (++n->retry > CFG_RETRY) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Config ke 0x%04x gagal setelah %d kali coba", n->addr, CFG_RETRY);
        ESP_LOGE(TAG, "%s", msg);
        emit_log("error", "PROV", msg);
        return;
    }
    ESP_LOGW(TAG, "Config ke 0x%04x belum berhasil, coba lagi (%u/%d)", n->addr, n->retry, CFG_RETRY);
    cfg_send(n);
}

static esp_err_t pump_dispatch_locked(uint16_t addr, bool on)
{
    esp_ble_mesh_client_common_param_t c = {0};
    esp_ble_mesh_generic_client_set_state_t s = {0};
    c.model = onoff_cli.model;
    c.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET;
    c.ctx.net_idx = MESH_NET_IDX;
    c.ctx.app_idx = MESH_APP_IDX;
    c.ctx.addr = addr;
    c.ctx.send_ttl = MSG_TTL;
    s.onoff_set.onoff = on;
    s.onoff_set.tid = tid++;

    pump_in_flight = true;
    pump_in_flight_val = on;

    esp_err_t err = esp_ble_mesh_generic_client_set_state(&c, &s);
    if (err != ESP_OK) {
        pump_in_flight = false;
    }
    return err;
}

static void pump_retry_cb(void *arg)
{
    bool schedule_fail = false;
    bool fail_val = false;
    bool retry_sent = false;
    bool val_to_send = false;
    uint8_t cur_retry = 0;

    xSemaphoreTake(pump_mux, portMAX_DELAY);
    if (pump_has_target && !pump_in_flight) {
        node_t *n = node_by_role(MESH_ROLE_PUMP);
        if (n && cfg_action(n) == ACT_END) {
            val_to_send = pump_target;
            cur_retry = pump_retry;
            esp_err_t err = pump_dispatch_locked(n->addr, val_to_send);
            if (err == ESP_OK) {
                retry_sent = true;
            } else if (pump_retry < PUMP_MAX_RETRY) {
                pump_retry++;
                esp_timer_start_once(pump_retry_timer, PUMP_RETRY_DELAY_US);
            } else {
                pump_has_target = false;
                schedule_fail = true;
                fail_val = val_to_send;
            }
        } else {
            pump_has_target = false;
        }
    }
    xSemaphoreGive(pump_mux);

    if (retry_sent) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Kirim ulang perintah pompa %s (percobaan %u/%d)",
                 val_to_send ? "ON" : "OFF", cur_retry, PUMP_MAX_RETRY);
        ESP_LOGW(TAG, "%s", msg);
        emit_log("warn", "PUMP", msg);
    } else if (schedule_fail) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Perintah pompa %s gagal dikirim ke Node 1", fail_val ? "ON" : "OFF");
        ESP_LOGE(TAG, "%s", msg);
        emit_log("error", "PUMP", msg);
        if (pump_cb) {
            pump_cb(fail_val, false, true);
        }
    }
}

static void generic_cb(esp_ble_mesh_generic_client_cb_event_t event,
                       esp_ble_mesh_generic_client_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT && param->error_code == ESP_OK) {
        bool actual = param->status_cb.onoff_status.present_onoff;
        bool need_followup = false;
        bool next_val = false;
        uint16_t pump_addr = 0;

        xSemaphoreTake(pump_mux, portMAX_DELAY);
        pump_in_flight = false;
        pump_retry = 0;
        if (pump_has_target && pump_target != actual) {
            node_t *n = node_by_role(MESH_ROLE_PUMP);
            if (n && cfg_action(n) == ACT_END) {
                need_followup = true;
                next_val = pump_target;
                pump_addr = n->addr;
            } else {
                pump_has_target = false;
            }
        } else {
            pump_has_target = false;
        }
        xSemaphoreGive(pump_mux);

        if (pump_cb) {
            pump_cb(actual, true, false);
        }

        if (need_followup) {
            xSemaphoreTake(pump_mux, portMAX_DELAY);
            esp_err_t err = pump_dispatch_locked(pump_addr, next_val);
            if (err != ESP_OK) {
                esp_timer_start_once(pump_retry_timer, PUMP_RETRY_DELAY_US);
            }
            xSemaphoreGive(pump_mux);

            char msg[96];
            snprintf(msg, sizeof(msg), "Lanjut kirim perintah pompa tertunda: %s", next_val ? "ON" : "OFF");
            ESP_LOGI(TAG, "%s", msg);
            emit_log("info", "PUMP", msg);
        }
        return;
    }

    if (event == ESP_BLE_MESH_GENERIC_CLIENT_TIMEOUT_EVT ||
        (event == ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT && param->error_code != ESP_OK)) {
        bool exhausted = false;
        bool target_val = false;
        uint8_t next_retry = 0;

        xSemaphoreTake(pump_mux, portMAX_DELAY);
        pump_in_flight = false;
        if (pump_has_target) {
            target_val = pump_target;
            if (pump_target != pump_in_flight_val) {
                /* Perintah baru menggantikan yang timeout: reset counter retry. */
                pump_retry = 0;
            }
            if (pump_retry < PUMP_MAX_RETRY) {
                pump_retry++;
                next_retry = pump_retry;
                esp_timer_stop(pump_retry_timer);
                esp_timer_start_once(pump_retry_timer, PUMP_RETRY_DELAY_US);
            } else {
                pump_has_target = false;
                exhausted = true;
            }
        }
        xSemaphoreGive(pump_mux);

        if (exhausted) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Pompa %s timeout setelah %d kali retry",
                     target_val ? "ON" : "OFF", PUMP_MAX_RETRY);
            ESP_LOGE(TAG, "%s", msg);
            emit_log("error", "PUMP", msg);
            if (pump_cb) {
                pump_cb(target_val, false, true);
            }
        } else if (next_retry > 0) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Perintah pompa %s timeout, jadwalkan retry %u/%d",
                     target_val ? "ON" : "OFF", next_retry, PUMP_MAX_RETRY);
            ESP_LOGW(TAG, "%s", msg);
            emit_log("warn", "PUMP", msg);
        }
    }
}

static void prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        const uint8_t *uuid = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        uint8_t role = uuid[1];
        if (uuid[0] != MESH_UUID_TAG ||
            (role != MESH_ROLE_PUMP && role != MESH_ROLE_SENSOR && role != MESH_ROLE_RELAY)) {
            break;
        }
        ESP_LOGI(TAG, "Perangkat terdeteksi, peran 0x%02x, alamat %02x:%02x:%02x:%02x:%02x:%02x", role,
                 uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7]);

        esp_ble_mesh_unprov_dev_add_t add = {0};
        memcpy(add.addr, param->provisioner_recv_unprov_adv_pkt.addr, BD_ADDR_LEN);
        add.addr_type = param->provisioner_recv_unprov_adv_pkt.addr_type;
        memcpy(add.uuid, uuid, 16);
        add.oob_info = param->provisioner_recv_unprov_adv_pkt.oob_info;
        add.bearer = param->provisioner_recv_unprov_adv_pkt.bearer;

        esp_err_t err = esp_ble_mesh_provisioner_add_unprov_dev(&add,
            ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Antre device gagal: %s", esp_err_to_name(err));
        }
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        uint16_t addr = param->provisioner_prov_complete.unicast_addr;
        node_t *n = node_add(param->provisioner_prov_complete.device_uuid, addr);
        if (!n) {
            ESP_LOGE(TAG, "Tabel node penuh, device 0x%04x diabaikan", addr);
            break;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "Provisioning sukses: peran 0x%02x, alamat 0x%04x", n->role, addr);
        ESP_LOGI(TAG, "%s", msg);
        emit_log("info", "PROV", msg);
        emit_status();
        cfg_send(n);
        break;
    }
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        if (param->provisioner_add_app_key_comp.err_code == ESP_OK) {
            esp_ble_mesh_provisioner_bind_app_key_to_local_model(OWN_ADDR, MESH_APP_IDX,
                ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, ESP_BLE_MESH_CID_NVAL);
            esp_ble_mesh_provisioner_bind_app_key_to_local_model(OWN_ADDR, MESH_APP_IDX,
                MESH_VND_MODEL_ID, MESH_CID);
        }
        break;
    case ESP_BLE_MESH_PROXY_SERVER_CONNECTED_EVT:
        proxy_connected = true;
        ESP_LOGI(TAG, "GATT Proxy client terhubung (conn %u)", param->proxy_server_connected.conn_handle);
        emit_log("info", "PROXY", "GATT Proxy client terhubung");
        emit_status();
        break;
    case ESP_BLE_MESH_PROXY_SERVER_DISCONNECTED_EVT:
        proxy_connected = false;
        ESP_LOGI(TAG, "GATT Proxy client terputus (reason 0x%02x)", param->proxy_server_disconnected.reason);
        emit_log("info", "PROXY", "GATT Proxy client terputus");
        emit_status();
        break;
    default:
        break;
    }
}

static void model_cb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT || param->model_operation.opcode != MESH_VND_OP_DATA) {
        return;
    }

    sensor_reading_t r;
    if (!sensor_decode(param->model_operation.msg, param->model_operation.length, &r)) {
        ESP_LOGW(TAG, "Payload sensor tidak valid, dibuang");
        emit_log("warn", "SENSOR", "Payload sensor tidak valid, dibuang");
        return;
    }

    uint16_t src = param->model_operation.ctx->addr;
    int8_t rssi = param->model_operation.ctx->recv_rssi;
    int64_t now = esp_timer_get_time();

    /* Deduplikasi anti-burst untuk retransmit atau relay yang lolos cache. */
    if (src == last_sensor_addr) {
        if (r.has_seq && last_sensor_has_seq && r.seq == last_sensor_seq &&
            (now - last_sensor_us) < 15000000LL) {
            ESP_LOGD(TAG, "Buang duplikat sensor #%u dari 0x%04x", r.seq, src);
            return;
        }
        if (!r.has_seq && (now - last_sensor_us) < 1500000LL) {
            ESP_LOGD(TAG, "Buang burst sensor (legacy) dari 0x%04x", src);
            return;
        }
    }
    last_sensor_addr = src;
    last_sensor_seq = r.seq;
    last_sensor_has_seq = r.has_seq;
    last_sensor_us = now;

    if (sensor_cb) {
        sensor_cb(&r, src, rssi);
    }
}

esp_err_t mesh_prov_start(mesh_sensor_cb_t on_sensor,
                          mesh_pump_cb_t on_pump,
                          mesh_log_cb_t on_log,
                          mesh_status_cb_t on_status)
{
    sensor_cb = on_sensor;
    pump_cb = on_pump;
    log_cb = on_log;
    status_cb = on_status;

    pump_mux = xSemaphoreCreateMutex();
    if (!pump_mux) {
        return ESP_ERR_NO_MEM;
    }
    esp_timer_create_args_t targs = {
        .callback = pump_retry_cb,
        .name = "pump_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &pump_retry_timer), TAG, "timer gagal");

    store_load();
    ESP_RETURN_ON_ERROR(mesh_bt_init(), TAG, "BT init gagal");
    mesh_make_uuid(dev_uuid, MESH_ROLE_PROV);

    ESP_RETURN_ON_ERROR(esp_ble_mesh_register_prov_callback(prov_cb), TAG, "");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_register_config_client_callback(cfg_cb), TAG, "");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_register_generic_client_callback(generic_cb), TAG, "");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_register_custom_model_callback(model_cb), TAG, "");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_init(&provision, &composition), TAG, "mesh init gagal");

    /* Hanya perangkat dengan tag proyek di UUID[0] yang di-provision otomatis. */
    uint8_t tag = MESH_UUID_TAG;
    ESP_RETURN_ON_ERROR(esp_ble_mesh_provisioner_set_dev_uuid_match(&tag, 1, 0, false), TAG, "");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT),
                        TAG, "");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_provisioner_add_local_app_key(store.app_key, MESH_NET_IDX, MESH_APP_IDX),
                        TAG, "");
    ESP_RETURN_ON_ERROR(esp_ble_mesh_model_subscribe_group_addr(OWN_ADDR, MESH_CID, MESH_VND_MODEL_ID,
                                                               MESH_GROUP_SENSOR), TAG, "");

    /* Aktifkan GATT Proxy Server pada Node 4 sesuai arsitektur Provisioner + Proxy + Gateway. */
    esp_err_t perr = esp_ble_mesh_proxy_gatt_enable();
    if (perr == ESP_OK) {
        proxy_enabled = true;
        ESP_LOGI(TAG, "GATT Proxy Server aktif");
    } else {
        proxy_enabled = false;
        ESP_LOGW(TAG, "GATT Proxy Server belum bisa diaktifkan: %s", esp_err_to_name(perr));
    }

    ESP_LOGI(TAG, "Provisioner + Proxy aktif (TX +20 dBm), memantau perangkat tag 0x%02x", MESH_UUID_TAG);
    return ESP_OK;
}

esp_err_t mesh_prov_set_pump(bool on)
{
    xSemaphoreTake(pump_mux, portMAX_DELAY);
    node_t *n = node_by_role(MESH_ROLE_PUMP);
    if (!n || cfg_action(n) != ACT_END) {
        xSemaphoreGive(pump_mux);
        return ESP_ERR_INVALID_STATE;
    }

    pump_target = on;
    pump_has_target = true;

    if (pump_in_flight) {
        /* Transaksi sebelumnya masih berjalan: target terbaru disimpan dan dikirim otomatis setelah selesai. */
        xSemaphoreGive(pump_mux);
        return ESP_OK;
    }

    pump_retry = 0;
    esp_timer_stop(pump_retry_timer);
    esp_err_t err = pump_dispatch_locked(n->addr, on);
    if (err != ESP_OK) {
        pump_retry = 1;
        esp_timer_start_once(pump_retry_timer, PUMP_RETRY_DELAY_US);
    }
    xSemaphoreGive(pump_mux);
    return ESP_OK;
}

void mesh_prov_get_status(mesh_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->proxy_enabled = proxy_enabled;
    out->proxy_connected = proxy_connected;
    for (int i = 0; i < MESH_MAX_NODES; i++) {
        if (store.nodes[i].used) {
            int idx = out->node_count++;
            out->nodes[idx].addr = store.nodes[i].addr;
            out->nodes[idx].role = store.nodes[i].role;
            out->nodes[idx].ready = (cfg_action(&store.nodes[i]) == ACT_END);
        }
    }
}

bool mesh_prov_pump_pending(bool *target_out)
{
    bool pending;
    xSemaphoreTake(pump_mux, portMAX_DELAY);
    pending = pump_has_target || pump_in_flight;
    if (target_out) {
        *target_out = pump_target;
    }
    xSemaphoreGive(pump_mux);
    return pending;
}
