#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "mesh_payload.h"

#define MESH_MAX_NODES 5

typedef struct {
    uint16_t addr;
    uint8_t role;
    bool ready;
} mesh_node_info_t;

typedef struct {
    bool proxy_enabled;
    bool proxy_connected;
    int node_count;
    mesh_node_info_t nodes[MESH_MAX_NODES];
} mesh_status_t;

typedef void (*mesh_sensor_cb_t)(const sensor_reading_t *r, uint16_t src_addr, int8_t rssi);
typedef void (*mesh_pump_cb_t)(bool on, bool confirmed, bool failed);
typedef void (*mesh_log_cb_t)(const char *level, const char *src, const char *msg);
typedef void (*mesh_status_cb_t)(void);

esp_err_t mesh_prov_start(mesh_sensor_cb_t on_sensor,
                          mesh_pump_cb_t on_pump,
                          mesh_log_cb_t on_log,
                          mesh_status_cb_t on_status);

/* Kirim perintah pompa dengan antrean desired-state dan auto-retry.
 * Mengembalikan ESP_ERR_INVALID_STATE jika pompa belum ter-provision / belum selesai config. */
esp_err_t mesh_prov_set_pump(bool on);

void mesh_prov_get_status(mesh_status_t *out);
bool mesh_prov_pump_pending(bool *target_out);
