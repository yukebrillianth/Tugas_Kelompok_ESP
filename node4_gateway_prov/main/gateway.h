#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "mesh_payload.h"
#include "mesh_prov.h"
#include "pump_logic.h"

typedef enum {
    GW_CMD_PUMP_ON,
    GW_CMD_PUMP_OFF,
    GW_CMD_MODE_AUTO,
    GW_CMD_SET_CFG,
} gw_cmd_type_t;

typedef struct {
    gw_cmd_type_t type;
    pump_cfg_t cfg;
} gw_cmd_t;

typedef void (*gw_cmd_cb_t)(const gw_cmd_t *cmd);
typedef void (*gw_connected_cb_t)(void);

esp_err_t gateway_start(gw_cmd_cb_t on_cmd, gw_connected_cb_t on_connected);

void gateway_publish_telemetry(const sensor_reading_t *r,
                               bool pump_on,
                               bool target_pump,
                               bool manual,
                               int8_t ble_rssi);

void gateway_publish_status(bool manual,
                            bool pump_on,
                            bool target_pump,
                            bool pending,
                            const pump_cfg_t *cfg,
                            const mesh_status_t *mesh);

void gateway_publish_log(const char *level, const char *src, const char *msg);
