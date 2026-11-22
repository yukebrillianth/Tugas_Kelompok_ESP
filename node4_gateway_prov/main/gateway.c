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

#define TAG                 "GATEWAY"
#define TOPIC_TELEMETRY     "iot/mesh/telemetry"
#define TOPIC_CMD           "iot/mesh/cmd"

static esp_mqtt_client_handle_t mqtt;
static gw_cmd_cb_t cmd_cb;

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
    char buf[128];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "Perintah bukan JSON: %s", buf);
        return;
    }

    const cJSON *pump = cJSON_GetObjectItem(root, "pump");
    const cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsNumber(pump)) {
        cmd_cb(pump->valueint ? GW_CMD_PUMP_ON : GW_CMD_PUMP_OFF);
    } else if (cJSON_IsString(mode) && strcmp(mode->valuestring, "auto") == 0) {
        cmd_cb(GW_CMD_MODE_AUTO);
    } else {
        ESP_LOGW(TAG, "Perintah tidak dikenal: %s", buf);
    }
    cJSON_Delete(root);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT terhubung ke %s", CONFIG_GW_MQTT_URI);
        esp_mqtt_client_subscribe(mqtt, TOPIC_CMD, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT putus, menyambung ulang");
        break;
    case MQTT_EVENT_DATA:
        handle_cmd(e->data, e->data_len);
        break;
    default:
        break;
    }
}

esp_err_t gateway_start(gw_cmd_cb_t on_cmd)
{
    cmd_cb = on_cmd;

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

void gateway_publish(const sensor_reading_t *r, bool pump_on)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ap.rssi = 0;
    }

    char msg[160];
    int len = snprintf(msg, sizeof(msg),
                       "{\"temp\": %.1f, \"hum\": %.1f, \"pump\": %d, \"rssi\": %d, \"uptime\": %lld}",
                       (double)r->temp_c, (double)r->hum_pct, pump_on ? 1 : 0, ap.rssi,
                       (long long)(esp_timer_get_time() / 1000000));

    if (esp_mqtt_client_publish(mqtt, TOPIC_TELEMETRY, msg, len, 0, 0) < 0) {
        ESP_LOGW(TAG, "Publish telemetri gagal, broker belum tersambung");
    } else {
        ESP_LOGI(TAG, "MQTT publish %s: %s", TOPIC_TELEMETRY, msg);
    }
}
