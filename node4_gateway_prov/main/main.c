#include "esp_log.h"
#include "nvs_flash.h"

#include "gateway.h"
#include "mesh_prov.h"
#include "pump_logic.h"

#define TAG "MAIN"

static bool manual;
static bool pump_on;

static void pump_command(bool on)
{
    esp_err_t err = mesh_prov_set_pump(on);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Perintah pompa %s belum bisa dikirim: %s", on ? "ON" : "OFF", esp_err_to_name(err));
    }
}

static void on_sensor(const sensor_reading_t *r)
{
    ESP_LOGI(TAG, "Sensor: suhu %d C, kelembapan %d %%", r->temp_c, r->hum_pct);
    gateway_publish(r, pump_on);

    if (manual) {
        return;
    }
    bool want = pump_decide(r->temp_c, pump_on, CONFIG_PUMP_ON_TEMP, CONFIG_PUMP_OFF_TEMP);
    if (want != pump_on) {
        ESP_LOGI(TAG, "Mode auto: suhu %d C, pompa diminta %s", r->temp_c, want ? "ON" : "OFF");
        pump_command(want);
    }
}

static void on_pump(bool on)
{
    pump_on = on;
    ESP_LOGI(TAG, "Status pompa: %s", on ? "ON" : "OFF");
}

static void on_cmd(gw_cmd_t cmd)
{
    switch (cmd) {
    case GW_CMD_PUMP_ON:
        manual = true;
        pump_command(true);
        break;
    case GW_CMD_PUMP_OFF:
        manual = true;
        pump_command(false);
        break;
    case GW_CMD_MODE_AUTO:
        manual = false;
        break;
    }
    ESP_LOGI(TAG, "Mode: %s", manual ? "manual" : "auto");
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(gateway_start(on_cmd));
    ESP_ERROR_CHECK(mesh_prov_start(on_sensor, on_pump));

    ESP_LOGI(TAG, "Siap. Pompa ON saat >= %d C, OFF saat < %d C", CONFIG_PUMP_ON_TEMP, CONFIG_PUMP_OFF_TEMP);
}
