#pragma once
/* Tanpa include ESP-IDF, supaya bisa dites di host. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Suhu (int8) lalu kelembapan. DHT11 hanya memberi bilangan bulat. */
#define SENSOR_PAYLOAD_LEN 2

typedef struct {
    int8_t temp_c;
    uint8_t hum_pct;
} sensor_reading_t;

static inline void sensor_encode(const sensor_reading_t *r, uint8_t out[SENSOR_PAYLOAD_LEN])
{
    out[0] = (uint8_t)r->temp_c;
    out[1] = r->hum_pct;
}

/* Input dari mesh, jadi panjang dan rentang divalidasi. */
static inline bool sensor_decode(const uint8_t *in, size_t len, sensor_reading_t *r)
{
    if (in == NULL || r == NULL || len != SENSOR_PAYLOAD_LEN) {
        return false;
    }
    int8_t t = (int8_t)in[0];
    if (t < -40 || t > 80 || in[1] > 100) {
        return false;
    }
    r->temp_c = t;
    r->hum_pct = in[1];
    return true;
}
