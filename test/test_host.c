/* Tes host, tanpa hardware. Jalankan dari root repo:
 *   gcc -Wall -Icommon/include -Inode4_gateway_prov/main test/test_host.c -o test/test_host && ./test/test_host
 * test/test_host sudah di .gitignore. */
#include <assert.h>
#include <stdio.h>

#include "mesh_payload.h"
#include "pump_logic.h"

int main(void)
{
    /* Payload v2 (3 byte dengan seq): round-trip */
    sensor_reading_t in = {.temp_c = 31, .hum_pct = 68, .seq = 42, .has_seq = true}, out = {0};
    uint8_t buf[SENSOR_PAYLOAD_LEN];
    sensor_encode(&in, buf);
    assert(sensor_decode(buf, sizeof buf, &out));
    assert(out.temp_c == 31 && out.hum_pct == 68 && out.seq == 42 && out.has_seq);

    /* Payload v1 (2 byte kompatibel mundur) */
    uint8_t legacy[SENSOR_PAYLOAD_LEN_V1] = {29, 70};
    assert(sensor_decode(legacy, sizeof legacy, &out));
    assert(out.temp_c == 29 && out.hum_pct == 70 && !out.has_seq);

    /* Payload: validasi di batas mesh */
    assert(!sensor_decode(buf, 1, &out));          /* panjang salah */
    assert(!sensor_decode(buf, 4, &out));          /* panjang berlebih */
    uint8_t hot[3] = {90, 50, 1};
    assert(!sensor_decode(hot, 3, &out));          /* suhu di luar rentang */
    uint8_t wet[3] = {30, 120, 1};
    assert(!sensor_decode(wet, 3, &out));          /* kelembapan > 100 */

    /* Pompa tanpa histeresis (on = off = 31) */
    assert(pump_decide(31, false, 31, 31));
    assert(!pump_decide(30, true, 31, 31));

    /* Pompa dengan histeresis (ON >= 29, OFF < 27) */
    assert(!pump_decide(28, false, 29, 27));       /* belum mencapai 29 */
    assert(pump_decide(29, false, 29, 27));        /* mencapai 29 -> ON */
    assert(pump_decide(28, true, 29, 27));         /* masih ON, fluktuasi 1 C tidak mematikan */
    assert(pump_decide(27, true, 29, 27));         /* masih ON, belum < 27 */
    assert(!pump_decide(26, true, 29, 27));        /* turun di bawah 27 -> OFF */

    /* Min hold mencegah pompa mati/nyala sebelum durasi tahan tercapai */
    pump_cfg_t cfg = pump_cfg_sanitize(29, 27, 15);
    assert(cfg.on_c == 29 && cfg.off_c == 27 && cfg.min_hold_s == 15);
    assert(pump_decide_hold(25, true, &cfg, 5));   /* baru 5 dtk ON, tetap ON */
    assert(!pump_decide_hold(25, true, &cfg, 15)); /* sudah 15 dtk, boleh OFF */
    assert(!pump_decide_hold(32, false, &cfg, 10));/* baru 10 dtk OFF, tetap OFF */
    assert(pump_decide_hold(32, false, &cfg, 20)); /* sudah 20 dtk, boleh ON */

    /* Sanitasi threshold tidak valid (off > on, out of range) */
    pump_cfg_t bad = pump_cfg_sanitize(80, 90, 999);
    assert(bad.on_c == 60 && bad.off_c == 60 && bad.min_hold_s == 300);
    pump_cfg_t low = pump_cfg_sanitize(-5, -10, -2);
    assert(low.on_c == 0 && low.off_c == 0 && low.min_hold_s == 0);

    printf("ALL HOST TESTS PASSED\n");
    return 0;
}
