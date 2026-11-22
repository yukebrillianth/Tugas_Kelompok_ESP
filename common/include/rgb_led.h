#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t rgb_led_init(void);

/* Nilai kecil sudah cukup terang untuk WS2812. */
void rgb_led_set(uint8_t r, uint8_t g, uint8_t b);
