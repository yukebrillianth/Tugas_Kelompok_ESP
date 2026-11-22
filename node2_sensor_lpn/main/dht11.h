#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "mesh_payload.h"

/* Modul DHT11 umumnya sudah punya pull-up. Sensor polos butuh resistor 4.7k ke VCC. */
bool dht11_read(gpio_num_t pin, sensor_reading_t *out);
