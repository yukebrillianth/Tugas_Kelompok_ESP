#pragma once
/* Tanpa include ESP-IDF, supaya bisa dites di host. */

#include <stdbool.h>

/* ON saat temp >= on_c, OFF saat temp < off_c. Jika off_c == on_c, tanpa histeresis. */
static inline bool pump_decide(int temp_c, bool pump_on, int on_c, int off_c)
{
    if (off_c > on_c) {
        off_c = on_c;
    }
    return pump_on ? (temp_c >= off_c) : (temp_c >= on_c);
}
