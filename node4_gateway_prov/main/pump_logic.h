#pragma once
/* Tanpa include ESP-IDF, supaya bisa dites di host. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int on_c;
    int off_c;
    int min_hold_s;
} pump_cfg_t;

/* Normalisasi konfigurasi threshold agar selalu valid. */
static inline pump_cfg_t pump_cfg_sanitize(int on_c, int off_c, int min_hold_s)
{
    if (on_c < 0) {
        on_c = 0;
    } else if (on_c > 60) {
        on_c = 60;
    }
    if (off_c < 0) {
        off_c = 0;
    } else if (off_c > on_c) {
        off_c = on_c;
    }
    if (min_hold_s < 0) {
        min_hold_s = 0;
    } else if (min_hold_s > 300) {
        min_hold_s = 300;
    }
    pump_cfg_t c = { .on_c = on_c, .off_c = off_c, .min_hold_s = min_hold_s };
    return c;
}

/* ON saat temp >= on_c, OFF saat temp < off_c. Jika off_c == on_c, tanpa histeresis. */
static inline bool pump_decide(int temp_c, bool pump_on, int on_c, int off_c)
{
    if (off_c > on_c) {
        off_c = on_c;
    }
    return pump_on ? (temp_c >= off_c) : (temp_c >= on_c);
}

/* Sama seperti pump_decide, tetapi menahan perubahan status jika elapsed_s < min_hold_s
 * (mencegah pompa mati/nyala bolak-balik akibat fluktuasi 1 C DHT11). */
static inline bool pump_decide_hold(int temp_c, bool pump_on, const pump_cfg_t *cfg, int64_t elapsed_s)
{
    bool want = pump_decide(temp_c, pump_on, cfg->on_c, cfg->off_c);
    if (want != pump_on && cfg->min_hold_s > 0 && elapsed_s >= 0 && elapsed_s < cfg->min_hold_s) {
        return pump_on;
    }
    return want;
}
