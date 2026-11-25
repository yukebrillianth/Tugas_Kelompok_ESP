#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "gateway.h"
#include "mesh_config.h"

#define TAG                 "GATEWAY"
#define TOPIC_TELEMETRY     "iot/mesh/telemetry"
#define TOPIC_STATUS        "iot/mesh/status"
#define TOPIC_LOG           "iot/mesh/log"
#define TOPIC_CMD           "iot/mesh/cmd"

static esp_mqtt_client_handle_t mqtt;
static gw_cmd_cb_t cmd_cb;
static gw_connected_cb_t connected_cb;
static volatile bool mqtt_ready;

static const char *role_name(uint8_t role)
{
    switch (role) {
    case MESH_ROLE_PUMP:   return "pump";
    case MESH_ROLE_SENSOR: return "sensor";
    case MESH_ROLE_RELAY:  return "relay";
    default:               return "unknown";
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi putus, menyambung ulang");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Wi-Fi tersambung, IP " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void handle_cmd(const char *data, int len)
{
    char buf[256];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "Perintah bukan JSON: %s", buf);
        gateway_publish_log("warn", "GW", "Perintah MQTT bukan JSON valid");
        return;
    }

    const cJSON *pump = cJSON_GetObjectItem(root, "pump");
    const cJSON *mode = cJSON_GetObjectItem(root, "mode");
    const cJSON *on_t = cJSON_GetObjectItem(root, "on_temp");
    const cJSON *off_t = cJSON_GetObjectItem(root, "off_temp");
    const cJSON *hold = cJSON_GetObjectItem(root, "min_hold_s");

    gw_cmd_t cmd = {0};
    if (cJSON_IsNumber(on_t) && cJSON_IsNumber(off_t)) {
        int hold_val = cJSON_IsNumber(hold) ? hold->valueint : CONFIG_PUMP_MIN_HOLD_S;
        cmd.type = GW_CMD_SET_CFG;
        cmd.cfg = pump_cfg_sanitize(on_t->valueint, off_t->valueint, hold_val);
        cmd_cb(&cmd);
    } else if (cJSON_IsNumber(pump)) {
        cmd.type = pump->valueint ? GW_CMD_PUMP_ON : GW_CMD_PUMP_OFF;
        cmd_cb(&cmd);
    } else if (cJSON_IsString(mode) && strcmp(mode->valuestring, "auto") == 0) {
        cmd.type = GW_CMD_MODE_AUTO;
        cmd_cb(&cmd);
    } else {
        ESP_LOGW(TAG, "Perintah tidak dikenal: %s", buf);
        gateway_publish_log("warn", "GW", "Perintah MQTT tidak dikenal");
    }
    cJSON_Delete(root);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_ready = true;
        ESP_LOGI(TAG, "MQTT terhubung ke %s", CONFIG_GW_MQTT_URI);
        esp_mqtt_client_subscribe(mqtt, TOPIC_CMD, 1);
        if (connected_cb) {
            connected_cb();
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_ready = false;
        ESP_LOGW(TAG, "MQTT putus, menyambung ulang");
        break;
    case MQTT_EVENT_DATA:
        handle_cmd(e->data, e->data_len);
        break;
    default:
        break;
    }
}

esp_err_t gateway_start(gw_cmd_cb_t on_cmd, gw_connected_cb_t on_connected)
{
    cmd_cb = on_cmd;
    connected_cb = on_connected;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif gagal");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop gagal");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init gagal");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event, NULL, NULL), TAG, "");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event, NULL, NULL), TAG, "");

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_GW_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_GW_WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start gagal");
    /* Maksimalkan TX power Wi-Fi ke 20 dBm (80 * 0.25 dBm). */
    esp_wifi_set_max_tx_power(80);

    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = CONFIG_GW_MQTT_URI,
    };
    mqtt = esp_mqtt_client_init(&mcfg);
    if (!mqtt) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL), TAG, "");
    return esp_mqtt_client_start(mqtt);
}

void gateway_publish_telemetry(const sensor_reading_t *r,
                               bool pump_on,
                               bool target_pump,
                               bool manual,
                               int8_t ble_rssi)
{
    if (!mqtt || !mqtt_ready) {
        return;
    }
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ap.rssi = 0;
    }

    char msg[240];
    int len = snprintf(msg, sizeof(msg),
                       "{\"temp\":%.1f,\"hum\":%.1f,\"pump\":%d,\"target_pump\":%d,"
                       "\"mode\":\"%s\",\"rssi\":%d,\"ble_rssi\":%d,\"seq\":%u,\"uptime\":%lld}",
                       (double)r->temp_c, (double)r->hum_pct,
                       pump_on ? 1 : 0, target_pump ? 1 : 0,
                       manual ? "manual" : "auto",
                       ap.rssi, (int)ble_rssi, (unsigned)r->seq,
                       (long long)(esp_timer_get_time() / 1000000));

    if (esp_mqtt_client_publish(mqtt, TOPIC_TELEMETRY, msg, len, 0, 0) < 0) {
        ESP_LOGW(TAG, "Publish telemetri gagal");
    } else {
        ESP_LOGI(TAG, "MQTT %s: %s", TOPIC_TELEMETRY, msg);
    }
}

void gateway_publish_status(bool manual,
                            bool pump_on,
                            bool target_pump,
                            bool pending,
                            const pump_cfg_t *cfg,
                            const mesh_status_t *mesh)
{
    if (!mqtt || !mqtt_ready || !cfg || !mesh) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "mode", manual ? "manual" : "auto");
    cJSON_AddNumberToObject(root, "pump", pump_on ? 1 : 0);
    cJSON_AddNumberToObject(root, "target_pump", target_pump ? 1 : 0);
    cJSON_AddBoolToObject(root, "pending", pending);
    cJSON_AddNumberToObject(root, "on_temp", cfg->on_c);
    cJSON_AddNumberToObject(root, "off_temp", cfg->off_c);
    cJSON_AddNumberToObject(root, "min_hold_s", cfg->min_hold_s);
    cJSON_AddBoolToObject(root, "proxy_enabled", mesh->proxy_enabled);
    cJSON_AddBoolToObject(root, "proxy_connected", mesh->proxy_connected);
    cJSON_AddNumberToObject(root, "tx_power_dbm", 20);
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));

    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    for (int i = 0; i < mesh->node_count; i++) {
        cJSON *item = cJSON_CreateObject();
        char addr_str[10];
        snprintf(addr_str, sizeof(addr_str), "0x%04x", mesh->nodes[i].addr);
        cJSON_AddStringToObject(item, "role", role_name(mesh->nodes[i].role));
        cJSON_AddStringToObject(item, "addr", addr_str);
        cJSON_AddBoolToObject(item, "ready", mesh->nodes[i].ready);
        cJSON_AddItemToArray(nodes, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        /* Retain=1 supaya dashboard langsung dapat threshold & status node saat dibuka. */
        esp_mqtt_client_publish(mqtt, TOPIC_STATUS, json, (int)strlen(json), 1, 1);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

void gateway_publish_log(const char *level, const char *src, const char *msg)
{
    if (!mqtt || !mqtt_ready || !msg) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddNumberToObject(root, "ts", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "level", level ? level : "info");
    cJSON_AddStringToObject(root, "src", src ? src : "GW");
    cJSON_AddStringToObject(root, "msg", msg);

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        esp_mqtt_client_publish(mqtt, TOPIC_LOG, json, (int)strlen(json), 0, 0);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}
