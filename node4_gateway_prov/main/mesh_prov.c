#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"

#include "mesh_bt.h"
#include "mesh_config.h"
#include "mesh_prov.h"

#define TAG         "PROV"
#define MAX_NODES   5
#define OWN_ADDR    0x0001
#define MSG_TTL     3
#define CFG_RETRY   3

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
    node_t nodes[MAX_NODES];
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
    ESP_BLE_MESH_MODEL_OP(MESH_VND_OP_DATA, SENSOR_PAYLOAD_LEN),
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
    for (int i = 0; i < MAX_NODES; i++) {
        if (store.nodes[i].used && store.nodes[i].addr == addr) {
            return &store.nodes[i];
        }
    }
    return NULL;
}

static node_t *node_by_role(uint8_t role)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (store.nodes[i].used && store.nodes[i].role == role) {
            return &store.nodes[i];
        }
    }
    return NULL;
}

static node_t *node_add(const uint8_t uuid[16], uint16_t addr)
{
    node_t *n = NULL;
    for (int i = 0; i < MAX_NODES && !n; i++) {
        if (store.nodes[i].used && memcmp(store.nodes[i].uuid, uuid, 16) == 0) {
            n = &store.nodes[i];
        }
    }
    for (int i = 0; i < MAX_NODES && !n; i++) {
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
        ESP_LOGI(TAG, "Node 0x%04x selesai dikonfigurasi", n->addr);
        store_save();
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
        s.model_pub_set.publish_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20);
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
        ESP_LOGE(TAG, "Config ke 0x%04x gagal setelah %d kali coba", n->addr, CFG_RETRY);
        return;
    }
    ESP_LOGW(TAG, "Config ke 0x%04x belum berhasil, coba lagi", n->addr);
    cfg_send(n);
}

static void generic_cb(esp_ble_mesh_generic_client_cb_event_t event,
                       esp_ble_mesh_generic_client_cb_param_t *param)
{
    if (param->error_code != ESP_OK) {
        ESP_LOGW(TAG, "Perintah ke pompa gagal (%d)", param->error_code);
        return;
    }
    if (event == ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT) {
        pump_cb(param->status_cb.onoff_status.present_onoff);
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
        ESP_LOGI(TAG, "Provisioning sukses: peran 0x%02x, alamat 0x%04x", n->role, addr);
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
        return;
    }
    sensor_cb(&r);
}

esp_err_t mesh_prov_start(mesh_sensor_cb_t on_sensor, mesh_pump_cb_t on_pump)
{
    sensor_cb = on_sensor;
    pump_cb = on_pump;

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

    ESP_LOGI(TAG, "Provisioner aktif, memantau perangkat tag 0x%02x", MESH_UUID_TAG);
    return ESP_OK;
}

esp_err_t mesh_prov_set_pump(bool on)
{
    node_t *n = node_by_role(MESH_ROLE_PUMP);
    if (!n || cfg_action(n) != ACT_END) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_ble_mesh_client_common_param_t c = {0};
    esp_ble_mesh_generic_client_set_state_t s = {0};
    c.model = onoff_cli.model;
    c.opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET;
    c.ctx.net_idx = MESH_NET_IDX;
    c.ctx.app_idx = MESH_APP_IDX;
    c.ctx.addr = n->addr;
    c.ctx.send_ttl = MSG_TTL;
    s.onoff_set.onoff = on;
    s.onoff_set.tid = tid++;

    return esp_ble_mesh_generic_client_set_state(&c, &s);
}
