#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t mesh_bt_init(void);

/* Panggil setelah mesh_bt_init(), karena alamat MAC baru tersedia setelah itu. */
void mesh_make_uuid(uint8_t uuid[16], uint8_t role);
