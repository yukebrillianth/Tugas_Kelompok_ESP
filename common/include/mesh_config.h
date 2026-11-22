#pragma once

#include "esp_ble_mesh_defs.h"

#define MESH_CID            0x02E5    /* CID contoh Espressif */
#define MESH_VND_MODEL_ID   0x0001
#define MESH_VND_OP_DATA    ESP_BLE_MESH_MODEL_OP_3(0x00, MESH_CID)

/* UUID: [0] tag proyek, [1] peran node, [2..7] MAC. Provisioner mencocokkan [0]. */
#define MESH_UUID_TAG       0xE5
#define MESH_ROLE_PROV      0x00
#define MESH_ROLE_PUMP      0x01
#define MESH_ROLE_SENSOR    0x02
#define MESH_ROLE_RELAY     0x03

#define MESH_GROUP_SENSOR   0xC001    /* sensor publish ke sini, gateway subscribe */
#define MESH_NET_IDX        ESP_BLE_MESH_KEY_PRIMARY
#define MESH_APP_IDX        0x0000
