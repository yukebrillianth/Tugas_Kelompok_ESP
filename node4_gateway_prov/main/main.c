#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "gateway.h"
#include "mesh_prov.h"
#include "pump_logic.h"

#define TAG "MAIN"

static SemaphoreHandle_t state_mux;
static bool manual;
static bool pump_on;
static int64_t last_pump_change_us = -1;
static pump_cfg_t pump_cfg;
static bool has_last_sensor;
static sensor_reading_t last_sensor;

static void cfg_load(void)
{
    pump_cfg = pump_cfg_sanitize(CONFIG_PUMP_ON_TEMP, CONFIG_PUMP_OFF_TEMP, CONFIG_PUMP_MIN_HOLD_S);

    nvs_handle_t h;
    if (nvs_open("pump_cfg", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t on_c = pump_cfg.on_c;
    int32_t off_c = pump_cfg.off_c;
    int32_t hold_s = pump_cfg.min_hold_s;
    nvs_get_i32(h, "on_c", &on_c);
    nvs_get_i32(h, "off_c", &off_c);
    nvs_get_i32(h, "hold_s", &hold_s);
    nvs_close(h);

    pump_cfg = pump_cfg_sanitize((int)on_c, (int)off_c, (int)hold_s);
}

static void cfg_save(const pump_cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open("pump_cfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, "on_c", c->on_c);
    nvs_set_i32(h, "off_c", c->off_c);
    nvs_set_i32(h, "hold_s", c->min_hold_s);
    nvs_commit(h);
    nvs_close(h);
}

static void publish_status_now(void)
{
    bool cur_manual;
    bool cur_pump;
    pump_cfg_t cur_cfg;

    xSemaphoreTake(state_mux, portMAX_DELAY);
    cur_manual = manual;
    cur_pump = pump_on;
    cur_cfg = pump_cfg;
    xSemaphoreGive(state_mux);

    bool target = cur_pump;
    bool pending = mesh_prov_pump_pending(&target);
    if (!pending) {
        target = cur_pump;
    }

    mesh_status_t ms;
    mesh_prov_get_status(&ms);
    gateway_publish_status(cur_manual, cur_pump, target, pending, &cur_cfg, &ms);
}

static void pump_command(bool on, const char *reason)
{
    esp_err_t err = mesh_prov_set_pump(on);
    char msg[128];
    if (err != ESP_OK) {
        snprintf(msg, sizeof(msg), "Perintah pompa %s belum bisa dikirim (%s): %s",
                 on ? "ON" : "OFF", reason, esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", msg);
        gateway_publish_log("warn", "PUMP", msg);
    } else {
        snprintf(msg, sizeof(msg), "Kirim perintah pompa %s (%s)", on ? "ON" : "OFF", reason);
        ESP_LOGI(TAG, "%s", msg);
        gateway_publish_log("info", "PUMP", msg);
    }
    publish_status_now();
}

static void on_sensor(const sensor_reading_t *r, uint16_t src_addr, int8_t rssi)
{
    ESP_LOGI(TAG, "Sensor 0x%04x [#%u]: suhu %d C, kelembapan %d %% (BLE %d dBm)",
             src_addr, r->seq, r->temp_c, r->hum_pct, rssi);

    bool cur_manual;
    bool cur_pump;
    pump_cfg_t cur_cfg;
    int64_t elapsed_s = -1;

    xSemaphoreTake(state_mux, portMAX_DELAY);
    last_sensor = *r;
    has_last_sensor = true;
    cur_manual = manual;
    cur_pump = pump_on;
    cur_cfg = pump_cfg;
    if (last_pump_change_us >= 0) {
        elapsed_s = (esp_timer_get_time() - last_pump_change_us) / 1000000LL;
    }
    xSemaphoreGive(state_mux);

    bool pending_target = cur_pump;
    bool pending = mesh_prov_pump_pending(&pending_target);

    if (!cur_manual) {
        bool want = pump_decide_hold(r->temp_c, cur_pump, &cur_cfg, elapsed_s);
        if (want != cur_pump && !(pending && pending_target == want)) {
            char reason[64];
            snprintf(reason, sizeof(reason), "auto: %d C, ON>=%d OFF<%d",
                     r->temp_c, cur_cfg.on_c, cur_cfg.off_c);
            pump_command(want, reason);
            pending = mesh_prov_pump_pending(&pending_target);
        }
    }

    gateway_publish_telemetry(r, cur_pump, pending ? pending_target : cur_pump, cur_manual, rssi);
}

static void on_pump(bool on, bool confirmed, bool failed)
{
    if (confirmed) {
        xSemaphoreTake(state_mux, portMAX_DELAY);
        if (pump_on != on || last_pump_change_us < 0) {
            last_pump_change_us = esp_timer_get_time();
        }
        pump_on = on;
        xSemaphoreGive(state_mux);

        char msg[64];
        snprintf(msg, sizeof(msg), "Status pompa terkonfirmasi: %s", on ? "ON" : "OFF");
        ESP_LOGI(TAG, "%s", msg);
        gateway_publish_log("info", "PUMP", msg);
    } else if (failed) {
        ESP_LOGE(TAG, "Konfirmasi pompa %s gagal", on ? "ON" : "OFF");
    }
    publish_status_now();
}

static void on_mesh_log(const char *level, const char *src, const char *msg)
{
    gateway_publish_log(level, src, msg);
}

static void on_mesh_status(void)
{
    publish_status_now();
}

static void on_mqtt_connected(void)
{
    publish_status_now();
    gateway_publish_log("info", "GW", "Gateway terhubung ke broker MQTT");
}

static void on_cmd(const gw_cmd_t *cmd)
{
    switch (cmd->type) {
    case GW_CMD_PUMP_ON:
        xSemaphoreTake(state_mux, portMAX_DELAY);
        manual = true;
        xSemaphoreGive(state_mux);
        pump_command(true, "manual");
        break;
    case GW_CMD_PUMP_OFF:
        xSemaphoreTake(state_mux, portMAX_DELAY);
        manual = true;
        xSemaphoreGive(state_mux);
        pump_command(false, "manual");
        break;
    case GW_CMD_MODE_AUTO: {
        bool do_eval = false;
        bool cur_pump = false;
        sensor_reading_t sr = {0};
        pump_cfg_t cur_cfg;

        xSemaphoreTake(state_mux, portMAX_DELAY);
        manual = false;
        cur_pump = pump_on;
        cur_cfg = pump_cfg;
        if (has_last_sensor) {
            sr = last_sensor;
            do_eval = true;
        }
        xSemaphoreGive(state_mux);

        ESP_LOGI(TAG, "Mode diubah ke AUTO");
        gateway_publish_log("info", "GW", "Mode diubah ke AUTO");
        if (do_eval) {
            bool want = pump_decide(sr.temp_c, cur_pump, cur_cfg.on_c, cur_cfg.off_c);
            if (want != cur_pump) {
                pump_command(want, "switch auto");
                break;
            }
        }
        publish_status_now();
        break;
    }
    case GW_CMD_SET_CFG: {
        xSemaphoreTake(state_mux, portMAX_DELAY);
        pump_cfg = cmd->cfg;
        xSemaphoreGive(state_mux);
        cfg_save(&cmd->cfg);

        char msg[96];
        snprintf(msg, sizeof(msg), "Threshold disimpan: ON >= %d C, OFF < %d C, hold %d dtk",
                 cmd->cfg.on_c, cmd->cfg.off_c, cmd->cfg.min_hold_s);
        ESP_LOGI(TAG, "%s", msg);
        gateway_publish_log("info", "CFG", msg);
        publish_status_now();
        break;
    }
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

    state_mux = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(state_mux ? ESP_OK : ESP_ERR_NO_MEM);
    cfg_load();

    ESP_ERROR_CHECK(gateway_start(on_cmd, on_mqtt_connected));
    ESP_ERROR_CHECK(mesh_prov_start(on_sensor, on_pump, on_mesh_log, on_mesh_status));

    ESP_LOGI(TAG, "Siap. Pompa ON >= %d C, OFF < %d C, min hold %d dtk",
             pump_cfg.on_c, pump_cfg.off_c, pump_cfg.min_hold_s);
}
