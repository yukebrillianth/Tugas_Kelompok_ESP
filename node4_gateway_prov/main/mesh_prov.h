#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "mesh_payload.h"

typedef void (*mesh_sensor_cb_t)(const sensor_reading_t *r);
typedef void (*mesh_pump_cb_t)(bool on);

esp_err_t mesh_prov_start(mesh_sensor_cb_t on_sensor, mesh_pump_cb_t on_pump);

/* ESP_ERR_INVALID_STATE jika pompa belum terpasang atau konfigurasinya belum selesai. */
esp_err_t mesh_prov_set_pump(bool on);
