#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "mesh_payload.h"

typedef enum {
    GW_CMD_PUMP_ON,
    GW_CMD_PUMP_OFF,
    GW_CMD_MODE_AUTO,
} gw_cmd_t;

typedef void (*gw_cmd_cb_t)(gw_cmd_t cmd);

esp_err_t gateway_start(gw_cmd_cb_t on_cmd);
void gateway_publish(const sensor_reading_t *r, bool pump_on);
