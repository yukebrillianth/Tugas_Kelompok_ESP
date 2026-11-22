/* Tes host, tanpa hardware. Jalankan dari root repo:
 *   gcc -Wall -Icommon/include -Inode4_gateway_prov/main test/test_host.c -o test/test_host && ./test/test_host
 * test/test_host sudah di .gitignore. */
#include <assert.h>
#include <stdio.h>

#include "mesh_payload.h"
#include "pump_logic.h"

int main(void)
{
    /* Payload: round-trip */
    sensor_reading_t in = {.temp_c = 31, .hum_pct = 68}, out = {0};
    uint8_t buf[SENSOR_PAYLOAD_LEN];
    sensor_encode(&in, buf);
    assert(sensor_decode(buf, sizeof buf, &out));
    assert(out.temp_c == 31 && out.hum_pct == 68);

    /* Payload: validasi di batas mesh */
    assert(!sensor_decode(buf, 1, &out));          /* panjang salah */
    uint8_t hot[2] = {90, 50};
    assert(!sensor_decode(hot, 2, &out));          /* suhu di luar rentang */
    uint8_t wet[2] = {30, 120};
    assert(!sensor_decode(wet, 2, &out));          /* kelembapan > 100 */

    /* Pompa tanpa histeresis (on = off = 31): sesuai brief */
    assert(pump_decide(31, false, 31, 31));
    assert(!pump_decide(30, true, 31, 31));

    /* Pompa dengan histeresis (ON >= 31, OFF < 30) */
    assert(!pump_decide(30, false, 31, 30));       /* belum mencapai 31 */
    assert(pump_decide(31, false, 31, 30));        /* mencapai 31 -> ON */
    assert(pump_decide(30, true, 31, 30));         /* masih ON, belum < 30 */
    assert(!pump_decide(29, true, 31, 30));        /* turun di bawah 30 -> OFF */

    printf("ALL HOST TESTS PASSED\n");
    return 0;
}
